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
#include "../tuxedo_io.h"

class DeviceInterface
{
public:
  explicit DeviceInterface( IO &io ) : m_io( io ) { }
  virtual ~DeviceInterface() { }

  virtual bool identify( bool &identified ) = 0;
  virtual bool deviceInterfaceIdStr( std::string &interfaceIdStr ) = 0;
  virtual bool deviceModelIdStr( std::string &modelIdStr ) = 0;
  virtual bool setEnableModeSet( bool enabled ) = 0;
  virtual bool getNumberFans( int &nrFans ) = 0;
  virtual bool setFansAuto() = 0;
  virtual bool setFanSpeedPercent( const int fanNr, const int fanSpeedPercent ) = 0;
  virtual bool getFanSpeedPercent( const int fanNr, int &fanSpeedPercent ) = 0;
  virtual bool getFanTemperature( const int fanNr, int &temperatureCelcius ) = 0;
  virtual bool getFansMinSpeed( int &minSpeed ) = 0;
  virtual bool getFansOffAvailable( bool &offAvailable ) = 0;
  virtual bool setWebcam( const bool status ) = 0;
  virtual bool getWebcam( bool &status ) = 0;
  virtual bool getAvailableODMPerformanceProfiles( std::vector< std::string > &profiles ) = 0;
  virtual bool setODMPerformanceProfile( std::string performanceProfile ) = 0;
  virtual bool getDefaultODMPerformanceProfile( std::string &profileName ) = 0;
  virtual bool getNumberTDPs( int &nrTDPs ) = 0;
  virtual bool getTDPDescriptors( std::vector< std::string > &tdpDescriptors ) = 0;
  virtual bool getTDPMin( const int tdpIndex, int &minValue ) = 0;
  virtual bool getTDPMax( const int tdpIndex, int &maxValue ) = 0;
  virtual bool setTDP( const int tdpIndex, const int tdpValue ) = 0;
  virtual bool getTDP( const int tdpIndex, int &tdpValue ) = 0;

protected:
  IO &m_io;
};
