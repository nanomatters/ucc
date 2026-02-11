/*!
 * Copyright (c) 2020-2022 TUXEDO Computers GmbH <tux@tuxedocomputers.com>
 *
 * This file is part of TUXEDO Control Center.
 *
 * TUXEDO Control Center is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * TUXEDO Control Center is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with TUXEDO Control Center.  If not, see <https://www.gnu.org/licenses/>.
 */
#pragma once

#include <string>
#include <vector>
#include <map>
#include <cmath>
#include "../tuxedo_io_ioctl.h"
#include "interface.h"

class ClevoDevice : public DeviceInterface
{
public:
  ClevoDevice( IO &io ) : DeviceInterface( io ) { }

  static inline bool canIdentify( IO &io, bool &identified )
  {
    int result;
    int ret = io.ioctlCall( R_HWCHECK_CL, result );
    identified = ( result == 1 );
    return ret;
  }

  virtual bool identify( bool &identified )
  { return canIdentify( m_io, identified ); }

  virtual bool deviceInterfaceIdStr( std::string &interfaceIdStr )
  { return m_io.ioctlCall( R_CL_HW_IF_STR, interfaceIdStr, 50 ); }

  virtual bool deviceModelIdStr( [[maybe_unused]] std::string &modelIdStr )
  { return false; }

  virtual bool setEnableModeSet( [[maybe_unused]] bool enabled )
  { return true; }

  virtual bool getFansMinSpeed( int &minSpeed )
  { minSpeed = 20; return true; }

  virtual bool getFansOffAvailable( bool &offAvailable )
  { offAvailable = true; return true; }

  virtual bool getNumberFans( int &nrFans )
  { nrFans = 3; return true; }

  virtual bool setFansAuto()
  {
    int argument = 0;
    argument |= 1;
    argument |= 1 << 0x01;
    argument |= 1 << 0x02;
    argument |= 1 << 0x03;
    return m_io.ioctlCall( W_CL_FANAUTO, argument );
  }

  virtual bool setFanSpeedPercent( const int fanNr, const int fanSpeedPercent )
  {
    int argument = 0;
    int fanSpeedRaw[ 3 ];
    int ret;

    if ( fanNr < 0 or fanNr >= 3 )
      return false;

    if ( fanSpeedPercent < 0 or fanSpeedPercent > 100 )
      return false;

    for ( int i = 0; i < 3; ++i )
    {
      if ( i == fanNr )
      {
        fanSpeedRaw[ i ] = static_cast< int >( std::round( fanSpeedPercent * 0xff / 100.0 ) );
      }
      else
      {
        ret = getFanSpeedRaw( i, fanSpeedRaw[ i ] );
        if ( not ret )
          return false;
      }
    }
    argument |= ( fanSpeedRaw[ 0 ] & 0xff );
    argument |= ( fanSpeedRaw[ 1 ] & 0xff ) << 0x08;
    argument |= ( fanSpeedRaw[ 2 ] & 0xff ) << 0x10;
    return m_io.ioctlCall( W_CL_FANSPEED, argument );
  }

  virtual bool getFanSpeedPercent( const int fanNr, int &fanSpeedPercent )
  {
    int fanSpeedRaw;
    int ret = getFanSpeedRaw( fanNr, fanSpeedRaw );
    if ( not ret )
      return false;

    fanSpeedPercent = static_cast< int >( std::round( ( static_cast< float >( fanSpeedRaw ) / static_cast< float >( MAX_FAN_SPEED ) ) * 100.0f ) );
    return ret;
  }

  virtual bool getFanTemperature( const int fanNr, int &temperatureCelcius )
  {
    int fanInfo;
    int ret = getFanInfo( fanNr, fanInfo );
    if ( not ret )
      return false;

    // Explicitly use temp2 since more consistently implemented
    int fanTemp2 = static_cast< int8_t >( ( fanInfo >> 0x10 ) & 0xff );
    temperatureCelcius = fanTemp2;

    // If a fan is not available a low value is read out
    if ( fanTemp2 <= 1 )
      ret = false;

    return ret;
  }

  virtual bool setWebcam( const bool status )
  {
    int argument = ( status ? 1 : 0 );
    return m_io.ioctlCall( W_CL_WEBCAM_SW, argument );
  }

  virtual bool getWebcam( bool &status )
  {
    int webcamStatus = 0;
    int ret = m_io.ioctlCall( R_CL_WEBCAM_SW, webcamStatus );
    status = ( webcamStatus == 1 ? true : false );
    return ret;
  }

  virtual bool getAvailableODMPerformanceProfiles( std::vector< std::string > &profiles )
  {
    profiles.clear();
    profiles.push_back( PERF_PROF_STR_QUIET );
    profiles.push_back( PERF_PROF_STR_POWERSAVE );
    profiles.push_back( PERF_PROF_STR_ENTERTAINMENT );
    profiles.push_back( PERF_PROF_STR_PERFORMANCE );
    return true;
  }

  virtual bool setODMPerformanceProfile( std::string performanceProfile )
  {
    bool result = false;
    bool perfProfileExists = ( clevoPerformanceProfilesToArgument.find( performanceProfile ) not_eq clevoPerformanceProfilesToArgument.end() );
    if ( perfProfileExists )
    {
      int perfProfileArgument = clevoPerformanceProfilesToArgument.at( performanceProfile );
      result = m_io.ioctlCall( W_CL_PERF_PROFILE, perfProfileArgument );
    }
    return result;
  }

  virtual bool getDefaultODMPerformanceProfile( std::string &profileName )
  { profileName = "performance"; return true; }

  virtual bool getNumberTDPs( [[maybe_unused]] int &nrTDPs )
  { return false; }

  virtual bool getTDPDescriptors( [[maybe_unused]] std::vector< std::string > &tdpDescriptors )
  { return false; }

  virtual bool getTDPMin( [[maybe_unused]] const int tdpIndex, [[maybe_unused]] int &minValue )
  { return false; }

  virtual bool getTDPMax( [[maybe_unused]] const int tdpIndex, [[maybe_unused]] int &maxValue )
  { return false; }

  virtual bool setTDP( [[maybe_unused]] const int tdpIndex, [[maybe_unused]] int tdpValue )
  { return false; }

  virtual bool getTDP( [[maybe_unused]] const int tdpIndex, [[maybe_unused]] int &tdpValue )
  { return false; }

private:
  const int MAX_FAN_SPEED = 0xff;
  const std::string PERF_PROF_STR_QUIET = "quiet";
  const std::string PERF_PROF_STR_POWERSAVE = "power_saving";
  const std::string PERF_PROF_STR_PERFORMANCE = "performance";
  const std::string PERF_PROF_STR_ENTERTAINMENT = "entertainment";

  const std::map< std::string, int > clevoPerformanceProfilesToArgument = {
    { PERF_PROF_STR_QUIET,          0x00 },
    { PERF_PROF_STR_POWERSAVE,      0x01 },
    { PERF_PROF_STR_PERFORMANCE,    0x02 },
    { PERF_PROF_STR_ENTERTAINMENT,  0x03 }
  };

  bool getFanInfo( int fanNr, int &fanInfo )
  {
    if ( fanNr < 0 or fanNr >= 3 )
      return false;

    bool result = false;
    int argument = 0;
    if ( fanNr == 0 )
    {
      result = m_io.ioctlCall( R_CL_FANINFO1, argument );
    }
    else if ( fanNr == 1 )
    {
      result = m_io.ioctlCall( R_CL_FANINFO2, argument );
    }
    else if ( fanNr == 2 )
    {
      result = m_io.ioctlCall( R_CL_FANINFO3, argument );
    }
    else if ( fanNr == 3 )
    {
      // result = IoctlCall(R_CL_FANINFO4, argument);
    }

    fanInfo = argument;
    return result;
  }

  bool getFanSpeedRaw( const int fanNr, int &fanSpeedRaw )
  {
    int fanInfo;
    int ret = getFanInfo( fanNr, fanInfo );
    fanSpeedRaw = ( fanInfo & 0xff );
    return ret;
  }
};
