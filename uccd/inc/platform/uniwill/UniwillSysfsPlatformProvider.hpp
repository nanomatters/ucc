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

#include "hal/IPlatformProvider.hpp"
#include "SysfsNode.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <syslog.h>
#include <vector>

namespace ucc::hal
{

/**
 * @brief Sysfs-based platform provider for Uniwill laptops.
 *
 * Reads TDP (cpu_pl1/pl2/pl4), fn_lock, and platform-profile directly
 * from the INOU0000 sysfs device exposed by the uniwill-laptop kernel
 * driver — no ioctl or /dev/tuxedo_io dependency.
 *
 * Priority 10 — same as the old TuxedoIOPlatformProvider.
 */
class UniwillSysfsPlatformProvider final : public IPlatformProvider
{
public:
  UniwillSysfsPlatformProvider() = default;

  std::string name() const override { return "uniwill-sysfs-platform"; }

  int priority() const override { return 10; }

  bool detect() override
  {
    m_basePath = discoverINOUDevice();
    if ( m_basePath.empty() )
      return false;

    syslog( LOG_INFO, "UniwillSysfsPlatformProvider: detected INOU device at %s",
            m_basePath.c_str() );
    return true;
  }

  HwCapability capabilities() const override
  {
    HwCapability caps = HwCapability::None;

    // TDP
    if ( SysfsNode< int32_t >( m_basePath + "/cpu_pl1" ).isAvailable() )
      caps |= HwCapability::CpuTdpControl;

    return caps;
  }

  // ----- TDP control via INOU sysfs -----

  int getNumberTDPs() override
  {
    int n = 0;
    if ( SysfsNode< int32_t >( m_basePath + "/cpu_pl1" ).isAvailable() ) ++n;
    if ( SysfsNode< int32_t >( m_basePath + "/cpu_pl2" ).isAvailable() ) ++n;
    if ( SysfsNode< int32_t >( m_basePath + "/cpu_pl4" ).isAvailable() ) ++n;
    return n;
  }

  std::vector< std::string > getTDPDescriptors() override
  {
    std::vector< std::string > descs;
    if ( SysfsNode< int32_t >( m_basePath + "/cpu_pl1" ).isAvailable() ) descs.emplace_back( "PL1" );
    if ( SysfsNode< int32_t >( m_basePath + "/cpu_pl2" ).isAvailable() ) descs.emplace_back( "PL2" );
    if ( SysfsNode< int32_t >( m_basePath + "/cpu_pl4" ).isAvailable() ) descs.emplace_back( "PL4" );
    return descs;
  }

  std::optional< int > getTDPMin( int index ) override
  {
    return readTDP( tdpMinSuffix( index ) );
  }

  std::optional< int > getTDPMax( int index ) override
  {
    return readTDP( tdpMaxSuffix( index ) );
  }

  std::optional< int > getTDP( int index ) override
  {
    return readTDP( tdpSuffix( index ) );
  }

  bool setTDP( int index, int value ) override
  {
    auto suffix = tdpSuffix( index );
    if ( suffix.empty() )
      return false;
    return SysfsNode< int32_t >( m_basePath + suffix ).write( static_cast< int32_t >( value ) );
  }

  // ----- Webcam — not supported on Uniwill -----

  std::optional< bool > getWebcam() override { return std::nullopt; }
  bool setWebcam( [[maybe_unused]] bool enabled ) override { return false; }

private:
  std::string m_basePath;

  static std::string discoverINOUDevice()
  {
    const std::string platformPath = "/sys/bus/platform/devices";
    std::error_code ec;
    for ( const auto &entry : std::filesystem::directory_iterator( platformPath, ec ) )
    {
      const std::string name = entry.path().filename().string();
      if ( name.rfind( "INOU0000:", 0 ) == 0 )
        return entry.path().string();
    }
    return {};
  }

  static std::string tdpSuffix( int index )
  {
    switch ( index )
    {
      case 0: return "/cpu_pl1";
      case 1: return "/cpu_pl2";
      case 2: return "/cpu_pl4";
      default: return {};
    }
  }

  static std::string tdpMinSuffix( int index )
  {
    switch ( index )
    {
      case 0: return "/cpu_pl1_min";
      case 1: return "/cpu_pl2_min";
      case 2: return "/cpu_pl4_min";
      default: return {};
    }
  }

  static std::string tdpMaxSuffix( int index )
  {
    switch ( index )
    {
      case 0: return "/cpu_pl1_max";
      case 1: return "/cpu_pl2_max";
      case 2: return "/cpu_pl4_max";
      default: return {};
    }
  }

  std::optional< int > readTDP( const std::string &suffix ) const
  {
    if ( suffix.empty() )
      return std::nullopt;
    auto val = SysfsNode< int32_t >( m_basePath + suffix ).read();
    if ( val.has_value() )
      return static_cast< int >( *val );
    return std::nullopt;
  }
};

} // namespace ucc::hal
