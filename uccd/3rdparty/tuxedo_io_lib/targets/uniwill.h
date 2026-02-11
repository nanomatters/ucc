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
#include <cerrno>
#include "../tuxedo_io_ioctl.h"
#include "interface.h"

class UniwillDevice : public DeviceInterface
{
public:
  UniwillDevice( IO &io ) : DeviceInterface( io ) { }

  static inline bool canIdentify( IO &io, bool &identified )
  {
    int result;
    int ret = io.ioctlCall( R_HWCHECK_UW, result );
    identified = ( result == 1 );
    return ret;
  }

  virtual bool identify( bool &identified )
  {
    return canIdentify( m_io, identified );
  }

  virtual bool deviceInterfaceIdStr( std::string &interfaceIdStr )
  {
    interfaceIdStr = "uniwill";
    return true;
  }

  virtual bool deviceModelIdStr( std::string &modelIdStr )
  {
    int32_t modelId;
    modelIdStr = "";
    bool success = m_io.ioctlCall( R_UW_MODEL_ID, modelId );
    if ( success )
    {
      modelIdStr = std::to_string( modelId );
    }
    return success;
  }

  virtual bool setEnableModeSet( bool enabled )
  {
    int enabledSet = ( enabled ? 0x01 : 0x00 );
    return m_io.ioctlCall( W_UW_MODE_ENABLE, enabledSet );
  }

  virtual bool getFansMinSpeed( int &minSpeed )
  {
    return m_io.ioctlCall( R_UW_FANS_MIN_SPEED, minSpeed );
  }

  virtual bool getFansOffAvailable( bool &offAvailable )
  {
    int result;
    int ret = m_io.ioctlCall( R_UW_FANS_OFF_AVAILABLE, result );
    offAvailable = ( result == 1 );
    return ret;
  }

  virtual bool getNumberFans( int &nrFans )
  {
    nrFans = 2;
    return true;
  }

  virtual bool setFansAuto()
  {
    return m_io.ioctlCall( W_UW_FANAUTO );
  }

  virtual bool setFanSpeedPercent( const int fanNr, const int fanSpeedPercent )
  {
    int fanSpeedRaw = static_cast< int >( std::round( static_cast< double >( MAX_FAN_SPEED ) * fanSpeedPercent / 100.0 ) );
    bool result;

    switch ( fanNr )
    {
      case 0:
        result = m_io.ioctlCall( W_UW_FANSPEED, fanSpeedRaw );
        break;
      case 1:
        result = m_io.ioctlCall( W_UW_FANSPEED2, fanSpeedRaw );
        break;
      default:
        result = false;
        break;
    }

    return result;
  }

  virtual bool getFanSpeedPercent( const int fanNr, int &fanSpeedPercent )
  {
    int fanSpeedRaw;
    bool result;

    switch ( fanNr )
    {
      case 0:
        result = m_io.ioctlCall( R_UW_FANSPEED, fanSpeedRaw );
        break;
      case 1:
        result = m_io.ioctlCall( R_UW_FANSPEED2, fanSpeedRaw );
        break;
      default:
        result = false;
        break;
    }

    // Calculate percentage - use ceiling for non-zero values to avoid showing 0% for low speeds
    if ( fanSpeedRaw > 0 )
    {
      double percentExact = static_cast< double >( fanSpeedRaw ) * 100.0 / static_cast< double >( MAX_FAN_SPEED );
      fanSpeedPercent = static_cast< int >( std::ceil( percentExact ) );
    }
    else
    {
      fanSpeedPercent = 0;
    }

    return result;
  }

  virtual bool getFanTemperature( const int fanNr, int &temperatureCelcius )
  {
    int temp = 0;
    bool result;

    switch ( fanNr )
    {
      case 0:
        result = m_io.ioctlCall( R_UW_FAN_TEMP, temp );
        break;
      case 1:
        result = m_io.ioctlCall( R_UW_FAN_TEMP2, temp );
        break;
      default:
        result = false;
        break;
    }

    temperatureCelcius = temp;

    // Also use known set value (0x00) from tccwmi to detect no temp/fan
    if ( temp == 0 )
    {
      result = false;
    }

    return result;
  }

  virtual bool setWebcam( [[maybe_unused]] const bool status )
  {
    return false;
  }

  virtual bool getWebcam( [[maybe_unused]] bool &status )
  {
    return false;
  }

  virtual bool getAvailableODMPerformanceProfiles( std::vector< std::string > &profiles )
  {
    int nrProfiles = 0;
    profiles.clear();
    bool result = m_io.ioctlCall( R_UW_PROFS_AVAILABLE, nrProfiles );
    if ( nrProfiles < 2 )
    {
      result = false;
    }
    if ( nrProfiles >= 2 )
    {
      profiles.push_back( PERF_PROF_STR_BALANCED );
      profiles.push_back( PERF_PROF_STR_ENTHUSIAST );
    }
    if ( nrProfiles >= 3 )
    {
      profiles.push_back( PERF_PROF_STR_OVERBOOST );
    }
    return result;
  }

  virtual bool setODMPerformanceProfile( std::string performanceProfile )
  {
    bool result = false;
    bool perfProfileExists = ( uniwillPerformanceProfilesToArgument.find( performanceProfile ) != uniwillPerformanceProfilesToArgument.end() );
    if ( perfProfileExists )
    {
      int32_t perfProfileArgument = uniwillPerformanceProfilesToArgument.at( performanceProfile );
      result = m_io.ioctlCall( W_UW_PERF_PROF, perfProfileArgument );
    }
    return result;
  }

  virtual bool getDefaultODMPerformanceProfile( std::string &profileName )
  {
    int nrProfiles = 0;
    int nrTDPs = 0;

    bool result = m_io.ioctlCall( R_UW_PROFS_AVAILABLE, nrProfiles );
    if ( result && nrProfiles > 0 )
    {
      getNumberTDPs( nrTDPs );
      if ( nrTDPs > 0 )
      {
        // LEDs only case (default to LEDs off)
        profileName = PERF_PROF_STR_OVERBOOST;
      }
      else
      {
        if ( nrProfiles > 2 )
        {
          profileName = PERF_PROF_STR_OVERBOOST;
        }
        else
        {
          profileName = PERF_PROF_STR_ENTHUSIAST;
        }
      }
    }
    else
    {
      result = false;
    }
    return result;
  }

  virtual bool getNumberTDPs( int &nrTDPs )
  {
    // Check return status of getters to figure out how many
    // TDPs are configurable
    for ( int i = 2; i >= 0; --i )
    {
      int status = 0;
      bool success = getTDP( i, status );
      if ( success && status >= 0 )
      {
        nrTDPs = i + 1;
        break;
      }
    }

    return true;
  }

  virtual bool getTDPDescriptors( std::vector< std::string > &tdpDescriptors )
  {
    int nrTDPs = 0;
    bool result = this->getNumberTDPs( nrTDPs );
    if ( nrTDPs >= 1 )
    {
      tdpDescriptors.push_back( "pl1" );
    }
    if ( nrTDPs >= 2 )
    {
      tdpDescriptors.push_back( "pl2" );
    }
    if ( nrTDPs >= 3 )
    {
      tdpDescriptors.push_back( "pl4" );
    }
    return result;
  }

  virtual bool getTDPMin( const int tdpIndex, int &minValue )
  {
    const unsigned long ioctl_tdp_min[] = { R_UW_TDP0_MIN, R_UW_TDP1_MIN, R_UW_TDP2_MIN };
    if ( tdpIndex < 0 || tdpIndex > 2 )
    {
      return -EINVAL;
    }

    return m_io.ioctlCall( ioctl_tdp_min[ tdpIndex ], minValue );
  }

  virtual bool getTDPMax( const int tdpIndex, int &maxValue )
  {
    const unsigned long ioctl_tdp_max[] = { R_UW_TDP0_MAX, R_UW_TDP1_MAX, R_UW_TDP2_MAX };
    if ( tdpIndex < 0 || tdpIndex > 2 )
    {
      return -EINVAL;
    }

    return m_io.ioctlCall( ioctl_tdp_max[ tdpIndex ], maxValue );
  }

  virtual bool setTDP( const int tdpIndex, int tdpValue )
  {
    const unsigned long ioctl_tdp_set[] = { W_UW_TDP0, W_UW_TDP1, W_UW_TDP2 };
    if ( tdpIndex < 0 || tdpIndex > 2 )
    {
      return -EINVAL;
    }

    return m_io.ioctlCall( ioctl_tdp_set[ tdpIndex ], tdpValue );
  }

  virtual bool getTDP( const int tdpIndex, int &tdpValue )
  {
    const unsigned long ioctl_tdp_get[] = { R_UW_TDP0, R_UW_TDP1, R_UW_TDP2 };
    if ( tdpIndex < 0 || tdpIndex > 2 )
    {
      return -EINVAL;
    }

    return m_io.ioctlCall( ioctl_tdp_get[ tdpIndex ], tdpValue );
  }

private:
  const int MAX_FAN_SPEED = 0xc8;
  const std::string PERF_PROF_STR_BALANCED = "power_save";
  const std::string PERF_PROF_STR_ENTHUSIAST = "enthusiast";
  const std::string PERF_PROF_STR_OVERBOOST = "overboost";

  const std::map< std::string, int > uniwillPerformanceProfilesToArgument = {
    { PERF_PROF_STR_BALANCED,       0x01 },
    { PERF_PROF_STR_ENTHUSIAST,     0x02 },
    { PERF_PROF_STR_OVERBOOST,      0x03 }
  };

  bool getFanInfo( int fanNr, int &fanInfo )
  {
    if ( fanNr < 0 || fanNr >= 3 )
    {
      return false;
    }

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
};
