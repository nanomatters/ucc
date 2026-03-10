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
#include "platform/uniwill/UniwillDefaultProfiles.hpp"
#include "SysfsNode.hpp"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <syslog.h>

// Forward declare — TuxedoIOAPI is only needed for model-ID detection.
class TuxedoIOAPI;

namespace ucc::hal
{

/**
 * @brief Profile provider for Uniwill / Clevo / TUXEDO laptops.
 *
 * Identifies the specific device via DMI SKU + WMI model ID and returns
 * the matching set of built-in default profiles from DefaultProfiles.cpp.
 *
 * Priority 100 — overrides the generic fallback when a Uniwill device
 * is detected.
 */
class UniwillProfileProvider : public IProfileProvider
{
public:
  explicit UniwillProfileProvider( TuxedoIOAPI &io )
    : m_io( io )
  {
  }

  std::string name() const override { return "UniwillProfileProvider"; }
  int priority() const override { return 100; }

  bool detect() override
  {
    m_deviceId = identifyDevice();
    m_detected = m_deviceId.has_value();
    if ( m_detected )
    {
      syslog( LOG_INFO, "[UniwillProfileProvider] Detected device %d",
              static_cast< int >( *m_deviceId ) );
    }
    return m_detected;
  }

  std::vector< UccProfile > getDefaultProfiles() const override
  {
    if ( m_deviceId.has_value() )
      return getUniwillDefaultProfiles( *m_deviceId );

    // Uniwill device detected but no per-device profile set — use generic Uniwill set
    return getUniwillFallbackDefaultProfiles();
  }

  UccProfile getDefaultCustomProfile() const override
  {
    if ( m_deviceId.has_value() )
      return getUniwillDefaultCustomProfile( *m_deviceId );
    return getUniwillDefaultCustomProfile();
  }

  std::vector< FanProfile > getDefaultFanProfiles(
    [[maybe_unused]] const std::vector< ucc::hal::FanZone > &zones ) const override
  {
    return getUniwillDefaultFanProfiles();
  }

  // --- Accessors for device-specific features ---

  std::optional< UniwillDeviceID > deviceId() const noexcept { return m_deviceId; }

  bool supportsWaterCooler() const noexcept
  {
    static const std::set< UniwillDeviceID > waterCoolerDevices = {
      UniwillDeviceID::STELLARIS1XI04,
      UniwillDeviceID::STEPOL1XA04,
      UniwillDeviceID::STELLARIS1XI05,
      UniwillDeviceID::STELLARIS16I06,
      UniwillDeviceID::STELLARIS17I06,
      UniwillDeviceID::STELLARIS16A07,
      UniwillDeviceID::XNE16A25,
      UniwillDeviceID::XNE16E25,
      UniwillDeviceID::STELLARIS16I07,
    };
    return m_deviceId.has_value() && waterCoolerDevices.count( *m_deviceId ) > 0;
  }

  bool supportsCTGPAdjustment() const noexcept
  {
    static const std::set< UniwillDeviceID > cTGPHiddenDevices = {
      UniwillDeviceID::IBP14G6_TUX,
      UniwillDeviceID::IBP14G6_TRX,
      UniwillDeviceID::IBP14G6_TQF,
      UniwillDeviceID::IBP14G7_AQF_ARX,
      UniwillDeviceID::IBPG8,
      UniwillDeviceID::IBPG10AMD,
    };
    return !m_deviceId.has_value() || cTGPHiddenDevices.count( *m_deviceId ) == 0;
  }

private:
  std::optional< UniwillDeviceID > identifyDevice()
  {
    const std::string dmiBasePath = "/sys/class/dmi/id";
    const std::string productSKU = SysfsNode< std::string >( dmiBasePath + "/product_sku" ).read().value_or( "" );

    std::string deviceModelId;
    m_io.deviceModelIdStr( deviceModelId );

    // DMI SKU → device map
    static const std::map< std::string, UniwillDeviceID > dmiSKUDeviceMap = {
      { "IBS1706",                         UniwillDeviceID::IBP17G6 },
      { "IBP1XI08MK1",                     UniwillDeviceID::IBPG8 },
      { "IBP1XI08MK2",                     UniwillDeviceID::IBPG8 },
      { "IBP14I08MK2",                     UniwillDeviceID::IBPG8 },
      { "IBP16I08MK2",                     UniwillDeviceID::IBPG8 },
      { "OMNIA08IMK2",                     UniwillDeviceID::IBPG8 },
      { "IBP14A10MK1 / IBP15A10MK1",      UniwillDeviceID::IBPG10AMD },
      { "IIBP14A10MK1 / IBP15A10MK1",     UniwillDeviceID::IBPG10AMD },
      { "POLARIS1XA02",                    UniwillDeviceID::POLARIS1XA02 },
      { "POLARIS1XI02",                    UniwillDeviceID::POLARIS1XI02 },
      { "POLARIS1XA03",                    UniwillDeviceID::POLARIS1XA03 },
      { "POLARIS1XI03",                    UniwillDeviceID::POLARIS1XI03 },
      { "STELLARIS1XA03",                  UniwillDeviceID::STELLARIS1XA03 },
      { "STEPOL1XA04",                     UniwillDeviceID::STEPOL1XA04 },
      { "STELLARIS1XI03",                  UniwillDeviceID::STELLARIS1XI03 },
      { "STELLARIS1XI04",                  UniwillDeviceID::STELLARIS1XI04 },
      { "PULSE1502",                       UniwillDeviceID::PULSE1502 },
      { "PULSE1403",                       UniwillDeviceID::PULSE1403 },
      { "PULSE1404",                       UniwillDeviceID::PULSE1404 },
      { "STELLARIS1XI05",                  UniwillDeviceID::STELLARIS1XI05 },
      { "POLARIS1XA05",                    UniwillDeviceID::POLARIS1XA05 },
      { "STELLARIS1XA05",                  UniwillDeviceID::STELLARIS1XA05 },
      { "STELLARIS16I06",                  UniwillDeviceID::STELLARIS16I06 },
      { "STELLARIS17I06",                  UniwillDeviceID::STELLARIS17I06 },
      { "STELLSL15A06",                    UniwillDeviceID::STELLSL15A06 },
      { "STELLSL15I06",                    UniwillDeviceID::STELLSL15I06 },
      { "AURA14GEN3",                      UniwillDeviceID::AURA14G3 },
      { "AURA15GEN3",                      UniwillDeviceID::AURA15G3 },
      { "STELLARIS16A07",                  UniwillDeviceID::STELLARIS16A07 },
      { "STELLARIS16I07",                  UniwillDeviceID::STELLARIS16I07 },
      { "XNE16A25",                        UniwillDeviceID::XNE16A25 },
      { "XNE16E25",                        UniwillDeviceID::XNE16E25 },
      { "GEMINI17I04",                     UniwillDeviceID::GEMINI17I04 },
      { "GEMINIGEN4I",                     UniwillDeviceID::GEMINI17I04 },
      { "IBM15A10",                        UniwillDeviceID::IBM15A10 },
      { "SIRIUS1601",                      UniwillDeviceID::SIRIUS1601 },
      { "SIRIUS1602",                      UniwillDeviceID::SIRIUS1602 },
    };

    if ( auto skuIt = dmiSKUDeviceMap.find( productSKU ); skuIt != dmiSKUDeviceMap.end() )
      return skuIt->second;

    // UWID (WMI interface) device mapping fallback
    static const std::map< int, UniwillDeviceID > uwidDeviceMap = {
      { 0x13, UniwillDeviceID::IBP14G6_TUX },
      { 0x12, UniwillDeviceID::IBP14G6_TRX },
      { 0x14, UniwillDeviceID::IBP14G6_TQF },
      { 0x17, UniwillDeviceID::IBP14G7_AQF_ARX },
    };

    int modelId = 0;
    try { modelId = std::stoi( deviceModelId ); }
    catch ( ... ) { }

    if ( auto uwidIt = uwidDeviceMap.find( modelId ); uwidIt != uwidDeviceMap.end() )
      return uwidIt->second;

    return std::nullopt;
  }

  TuxedoIOAPI &m_io;
  bool m_detected = false;
  std::optional< UniwillDeviceID > m_deviceId;
};

} // namespace ucc::hal
