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
#include "hal/ITempProvider.hpp"
#include "hal/IPlatformProvider.hpp"
#include "tuxedo_io_lib/tuxedo_io_api.hh"

#include <cmath>
#include <optional>
#include <string>
#include <syslog.h>
#include <vector>

namespace ucc::hal
{

/**
 * @brief TuxedoIO-based fan provider for Clevo/Uniwill laptops.
 *
 * Wraps the existing TuxedoIOAPI (ioctl-based) as an IFanProvider +
 * ITempProvider so that Uniwill/Clevo hardware continues to work
 * unchanged through the new HAL.
 *
 * Priority 10 — takes precedence over generic HwmonFanProvider when
 * /dev/tuxedo_io is available.
 */
class TuxedoIOFanProvider final : public IFanProvider
{
public:
  explicit TuxedoIOFanProvider( TuxedoIOAPI &io )
    : m_io( io )
  {}

  std::string name() const override { return "tuxedo-io"; }
  int priority() const override { return 10; } // OEM driver, highest priority

  bool detect() override
  {
    m_fans.clear();

    if ( !m_io.wmiAvailable() )
      return false;

    int nrFans = 0;
    bool detected = m_io.getNumberFans( nrFans ) && nrFans > 0;

    // Fallback: try reading temperature from fan 0
    if ( !detected )
    {
      int temp = -1;
      if ( m_io.getFanTemperature( 0, temp ) && temp >= 0 )
      {
        nrFans = 2;
        detected = true;
      }
    }

    if ( !detected )
      return false;

    // Get hardware limits
    m_io.getFansMinSpeed( m_minSpeed );
    {
      bool off = false;
      m_io.getFansOffAvailable( off );
      m_fansOffAvailable = off;
    }

    for ( int i = 0; i < nrFans; ++i )
    {
      FanInfo fi;
      fi.id = "tuxedio_fan" + std::to_string( i );
      fi.label = ( i == 0 ) ? "CPU Fan" : "GPU Fan " + std::to_string( i );
      fi.index = i;
      fi.canRead = true;
      fi.canControl = true;
      fi.deviceType = FanDeviceType::Staged; // laptop WMI-controlled fans
      m_fans.push_back( std::move( fi ) );
    }

    syslog( LOG_INFO, "TuxedoIOFanProvider: detected %d fans via /dev/tuxedo_io", nrFans );
    return true;
  }

  std::vector< FanInfo > enumerateFans() override { return m_fans; }

  std::optional< int > getFanRPM( [[maybe_unused]] const FanInfo &fan ) override
  {
    // TuxedoIO doesn't expose raw RPM, only percent
    return std::nullopt;
  }

  std::optional< int > getFanSpeedPercent( const FanInfo &fan ) override
  {
    int pct = 0;
    if ( m_io.getFanSpeedPercent( fan.index, pct ) )
      return pct;
    return std::nullopt;
  }

  bool setFanSpeedPercent( const FanInfo &fan, int percent ) override
  {
    return m_io.setFanSpeedPercent( fan.index, percent );
  }

  bool setFanAuto( [[maybe_unused]] const FanInfo &fan ) override
  {
    return m_io.setFansAuto();
  }

  void restoreAllAuto() override
  {
    m_io.setFansAuto();
  }

  int getMinSpeedPercent( [[maybe_unused]] const FanInfo &fan ) override
  {
    return m_minSpeed;
  }

  bool canTurnOff( [[maybe_unused]] const FanInfo &fan ) override
  {
    return m_fansOffAvailable;
  }

  /// Provide access to raw TuxedoIOAPI for legacy code paths
  TuxedoIOAPI &rawIO() { return m_io; }

private:
  TuxedoIOAPI &m_io;
  std::vector< FanInfo > m_fans;
  int m_minSpeed = 0;
  bool m_fansOffAvailable = false;
};

/**
 * @brief TuxedoIO-based temperature provider for Clevo/Uniwill laptops.
 *
 * Reads fan-associated temperatures (CPU/GPU) from /dev/tuxedo_io ioctls.
 */
class TuxedoIOTempProvider final : public ITempProvider
{
public:
  explicit TuxedoIOTempProvider( TuxedoIOAPI &io )
    : m_io( io )
  {}

  std::string name() const override { return "tuxedo-io-temps"; }

  bool detect() override
  {
    m_sensors.clear();

    if ( !m_io.wmiAvailable() )
      return false;

    // Probe available temperature sensors
    for ( int i = 0; i < 3; ++i )
    {
      int temp = -1;
      if ( m_io.getFanTemperature( i, temp ) && temp > 0 )
      {
        TempSensorInfo si;
        si.id = "tuxedio_temp" + std::to_string( i );
        si.label = ( i == 0 ) ? "CPU (WMI)" : "GPU " + std::to_string( i ) + " (WMI)";
        si.source = "tuxedo-io";
        si.index = i;
        m_sensors.push_back( std::move( si ) );
      }
    }

    return !m_sensors.empty();
  }

  std::vector< TempSensorInfo > enumerateSensors() override { return m_sensors; }

  std::optional< double > readTempCelsius( const TempSensorInfo &sensor ) override
  {
    int temp = -1;
    if ( m_io.getFanTemperature( sensor.index, temp ) && temp > 0 )
      return static_cast< double >( temp );
    return std::nullopt;
  }

private:
  TuxedoIOAPI &m_io;
  std::vector< TempSensorInfo > m_sensors;
};

/**
 * @brief TuxedoIO-based platform provider for Clevo/Uniwill laptops.
 *
 * Wraps OEM-specific features: ODM profiles, TDP, webcam, mode set.
 */
class TuxedoIOPlatformProvider final : public IPlatformProvider
{
public:
  explicit TuxedoIOPlatformProvider( TuxedoIOAPI &io )
    : m_io( io )
  {}

  std::string name() const override { return "tuxedo-io-platform"; }

  int priority() const override { return 10; }

  bool detect() override
  {
    return m_io.wmiAvailable();
  }

  HwCapability capabilities() const override
  {
    HwCapability caps = HwCapability::None;

    // Check OEM profiles
    std::vector< std::string > profiles;
    if ( const_cast< TuxedoIOAPI & >( m_io ).getAvailableODMPerformanceProfiles( profiles ) && !profiles.empty() )
      caps |= HwCapability::OdmProfiles;

    // Check TDP control
    int nrTdps = 0;
    if ( const_cast< TuxedoIOAPI & >( m_io ).getNumberTDPs( nrTdps ) && nrTdps > 0 )
      caps |= HwCapability::CpuTdpControl;

    // Check webcam
    bool webcamStatus = false;
    if ( const_cast< TuxedoIOAPI & >( m_io ).getWebcam( webcamStatus ) )
      caps |= HwCapability::WebcamSwitch;

    return caps;
  }

  std::vector< std::string > getOdmProfiles() override
  {
    std::vector< std::string > profiles;
    m_io.getAvailableODMPerformanceProfiles( profiles );
    return profiles;
  }

  bool setOdmProfile( const std::string &profile ) override
  {
    return m_io.setODMPerformanceProfile( profile );
  }

  std::string getDefaultOdmProfile() override
  {
    std::string name;
    m_io.getDefaultODMPerformanceProfile( name );
    return name;
  }

  std::optional< bool > getWebcam() override
  {
    bool status = false;
    if ( m_io.getWebcam( status ) )
      return status;
    return std::nullopt;
  }

  bool setWebcam( bool enabled ) override
  {
    return m_io.setWebcam( enabled );
  }

  int getNumberTDPs() override
  {
    int n = 0;
    m_io.getNumberTDPs( n );
    return n;
  }

  std::vector< std::string > getTDPDescriptors() override
  {
    std::vector< std::string > descs;
    m_io.getTDPDescriptors( descs );
    return descs;
  }

  std::optional< int > getTDPMin( int index ) override
  {
    int val = 0;
    if ( m_io.getTDPMin( index, val ) )
      return val;
    return std::nullopt;
  }

  std::optional< int > getTDPMax( int index ) override
  {
    int val = 0;
    if ( m_io.getTDPMax( index, val ) )
      return val;
    return std::nullopt;
  }

  std::optional< int > getTDP( int index ) override
  {
    int val = 0;
    if ( m_io.getTDP( index, val ) )
      return val;
    return std::nullopt;
  }

  bool setTDP( int index, int value ) override
  {
    return m_io.setTDP( index, value );
  }

  /// Provide access to raw API for legacy code that needs setEnableModeSet etc.
  TuxedoIOAPI &rawIO() { return m_io; }

private:
  TuxedoIOAPI &m_io;
};

} // namespace ucc::hal
