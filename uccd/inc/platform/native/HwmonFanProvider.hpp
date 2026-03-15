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

#include "hal/IFanProvider.hpp"
#include "SysfsNode.hpp"

#include <map>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <syslog.h>
#include <vector>

namespace ucc::hal
{

/**
 * @brief Generic hwmon-based fan monitoring and PWM control provider.
 *
 * Scans /sys/class/hwmon/ for chips that expose fan*_input and pwm*
 * sysfs files (nct67xx, it87, nzxt-smart2, etc.).  Works on any
 * Linux machine without OEM-specific kernel modules.
 *
 * PWM control semantics:
 *   pwmN_enable = 0  →  full speed (off = no control = BIOS default)
 *   pwmN_enable = 1  →  manual PWM (this provider writes pwmN)
 *   pwmN_enable = 2  →  automatic / thermal cruise
 *   pwmN_enable = 5  →  smart-fan mode (some Nuvoton chips)
 */
class HwmonFanProvider final : public IFanProvider
{
public:
  HwmonFanProvider() = default;

  std::string name() const override { return m_name; }
  int priority() const override { return 5; } // generic, below OEM drivers

  bool detect() override
  {
    m_fans.clear();
    m_name = "hwmon";

    namespace fs = std::filesystem;
    const fs::path hwmonBase = "/sys/class/hwmon";
    if ( !fs::exists( hwmonBase ) )
      return false;

    for ( const auto &entry : fs::directory_iterator( hwmonBase ) )
    {
      const fs::path hwmonDir = fs::canonical( entry.path() );
      std::string chipName = readHwmonName( hwmonDir );
      if ( chipName.empty() )
        continue;

      // Look for fan*_input / pwm* pairs
      for ( int i = 1; i <= 16; ++i )
      {
        const std::string fanInputPath = hwmonDir.string() + "/fan" + std::to_string( i ) + "_input";
        const std::string pwmPath      = hwmonDir.string() + "/pwm" + std::to_string( i );
        const std::string pwmEnPath    = pwmPath + "_enable";

        if ( !fs::exists( fanInputPath ) )
          continue;

        FanInfo fi;
        fi.id = entry.path().filename().string() + "_fan" + std::to_string( i );
        fi.sourceName = resolveSourceName( chipName );
        fi.hwmonPath = hwmonDir.string();
        fi.index = i;
        fi.canRead = true;
        fi.canControl = fs::exists( pwmPath ) && fs::exists( pwmEnPath );

        // Try to read label
        const std::string labelPath = hwmonDir.string() + "/fan" + std::to_string( i ) + "_label";
        if ( fs::exists( labelPath ) )
        {
          fi.label = readLine( labelPath );
        }
        if ( fi.label.empty() )
        {
          fi.label = chipName + " Fan " + std::to_string( i );
        }

        // Classify the device type based on label heuristics
        fi.deviceType = classifyFanByLabel( fi.label );

        m_fans.push_back( std::move( fi ) );
      }
    }

    if ( !m_fans.empty() )
    {
      // Name the provider after the first chip found
      std::string firstChip = readHwmonName( m_fans.front().hwmonPath );
      if ( !firstChip.empty() )
        m_name = "hwmon-" + firstChip;

      syslog( LOG_INFO, "HwmonFanProvider: detected %zu fans via %s",
              m_fans.size(), m_name.c_str() );
    }

    return !m_fans.empty();
  }

  std::vector< FanInfo > enumerateFans() override
  {
    return m_fans;
  }

  std::optional< int > getFanRPM( const FanInfo &fan ) override
  {
    const std::string path = fan.hwmonPath + "/fan" + std::to_string( fan.index ) + "_input";
    SysfsNode< int32_t > node( path );
    auto val = node.read();
    if ( val.has_value() )
      return static_cast< int >( *val );
    return std::nullopt;
  }

  std::optional< int > getFanSpeedPercent( const FanInfo &fan ) override
  {
    if ( !fan.canControl )
    {
      // No PWM file → can't determine percent, just return RPM-based estimate
      return std::nullopt;
    }

    const std::string pwmPath = fan.hwmonPath + "/pwm" + std::to_string( fan.index );
    SysfsNode< int32_t > node( pwmPath );
    auto val = node.read();
    if ( val.has_value() )
    {
      // PWM range is 0-255 → convert to 0-100
      int pct = ( *val * 100 + 127 ) / 255;
      return std::clamp( pct, 0, 100 );
    }
    return std::nullopt;
  }

  bool setFanSpeedPercent( const FanInfo &fan, int percent ) override
  {
    if ( !fan.canControl )
      return false;

    if ( percent < 0 || percent > 100 )
      return false;

    // Ensure we are in manual mode
    if ( !setManualMode( fan ) )
      return false;

    // Convert 0-100 → 0-255
    int pwmVal = ( percent * 255 + 50 ) / 100;
    pwmVal = std::clamp( pwmVal, 0, 255 );

    const std::string pwmPath = fan.hwmonPath + "/pwm" + std::to_string( fan.index );
    SysfsNode< int32_t > node( pwmPath );
    return node.write( static_cast< int32_t >( pwmVal ) );
  }

  bool setFanAuto( const FanInfo &fan ) override
  {
    if ( !fan.canControl )
      return false;

    const std::string enablePath = fan.hwmonPath + "/pwm" + std::to_string( fan.index ) + "_enable";
    SysfsNode< int32_t > node( enablePath );

    // Restore to automatic mode.
    // Try mode 2 (thermal cruise / auto) first.
    // If the original mode was 5 (smart-fan), we store and restore it.
    int mode = m_originalPwmEnable.count( fan.id ) ? m_originalPwmEnable[fan.id] : 2;
    if ( mode == 1 || mode == 0 )
      mode = 2; // don't restore manual/off modes
    return node.write( static_cast< int32_t >( mode ) );
  }

  void restoreAllAuto() override
  {
    for ( const auto &fan : m_fans )
    {
      if ( fan.canControl )
        setFanAuto( fan );
    }
    m_originalPwmEnable.clear();
  }

  int getMinSpeedPercent( [[maybe_unused]] const FanInfo &fan ) override { return 0; }
  bool canTurnOff( [[maybe_unused]] const FanInfo &fan ) override { return true; }

private:
  std::vector< FanInfo > m_fans;
  std::string m_name = "hwmon";

  // Track original pwm_enable value so we can restore on exit
  std::map< std::string, int > m_originalPwmEnable;

  bool setManualMode( const FanInfo &fan )
  {
    const std::string enablePath = fan.hwmonPath + "/pwm" + std::to_string( fan.index ) + "_enable";
    SysfsNode< int32_t > node( enablePath );

    auto current = node.read();
    if ( !current.has_value() )
      return false;

    // Save original mode if not already saved
    if ( m_originalPwmEnable.find( fan.id ) == m_originalPwmEnable.end() )
      m_originalPwmEnable[fan.id] = *current;

    // Already in manual mode
    if ( *current == 1 )
      return true;

    // Switch to manual
    return node.write( 1 );
  }

  static std::string readHwmonName( const std::filesystem::path &hwmonDir )
  {
    return readLine( ( hwmonDir / "name" ).string() );
  }

  static std::string trimCopy( const std::string &in )
  {
    size_t b = 0;
    while ( b < in.size() && std::isspace( static_cast< unsigned char >( in[b] ) ) )
      ++b;
    size_t e = in.size();
    while ( e > b && std::isspace( static_cast< unsigned char >( in[e - 1] ) ) )
      --e;
    return in.substr( b, e - b );
  }

  static std::string readFirstLine( const std::string &path )
  {
    std::ifstream in( path );
    if ( !in.is_open() )
      return {};
    std::string line;
    std::getline( in, line );
    return trimCopy( line );
  }

  static std::string readBoardDisplayName()
  {
    const std::string boardVendor = readFirstLine( "/sys/class/dmi/id/board_vendor" );
    const std::string boardName = readFirstLine( "/sys/class/dmi/id/board_name" );
    const std::string productName = readFirstLine( "/sys/class/dmi/id/product_name" );

    if ( !boardVendor.empty() && !boardName.empty() )
      return boardVendor + " " + boardName;
    if ( !boardName.empty() )
      return boardName;
    if ( !productName.empty() )
      return productName;
    return {};
  }

  static std::string resolveSourceName( const std::string &chipName )
  {
    const std::string lower = [&]() {
      std::string s = chipName;
      std::transform( s.begin(), s.end(), s.begin(),
                      []( unsigned char c ) { return static_cast< char >( std::tolower( c ) ); } );
      return s;
    }();

    const bool looksLikeMainboardController = lower.rfind( "nct", 0 ) == 0
                                           || lower.rfind( "it", 0 ) == 0
                                           || lower.rfind( "w836", 0 ) == 0
                                           || lower.rfind( "f718", 0 ) == 0
                                           || lower.find( "superio" ) != std::string::npos
                                           || lower == "acpi_ec";

    if ( looksLikeMainboardController )
    {
      const std::string boardName = readBoardDisplayName();
      if ( !boardName.empty() )
        return boardName;
    }

    return chipName.empty() ? std::string( "Board" ) : chipName;
  }

  static std::string readLine( const std::string &path )
  {
    std::ifstream file( path );
    if ( !file.is_open() )
      return {};
    std::string line;
    std::getline( file, line );
    // Trim trailing whitespace
    while ( !line.empty() && ( line.back() == '\n' || line.back() == '\r' || line.back() == ' ' ) )
      line.pop_back();
    return line;
  }

  /// Classify fan device type from its label string.
  static FanDeviceType classifyFanByLabel( const std::string &label )
  {
    // Case-insensitive substring match
    auto contains = []( const std::string &hay, const char *needle ) {
      std::string lower = hay;
      for ( auto &c : lower ) c = static_cast< char >( std::tolower( static_cast< unsigned char >( c ) ) );
      return lower.find( needle ) != std::string::npos;
    };

    if ( contains( label, "pump" ) )
      return FanDeviceType::Pump;

    return FanDeviceType::Fan;
  }
};

} // namespace ucc::hal
