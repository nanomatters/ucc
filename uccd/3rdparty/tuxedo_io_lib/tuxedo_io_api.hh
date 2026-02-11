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

#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <cmath>
#include "tuxedo_io_ioctl.h"
#include "tuxedo_io.h"
#include "targets/interface.h"
#include "targets/clevo.h"
#include "targets/uniwill.h"
#include "targets/dummy.h"

#define TUXEDO_IO_DEVICE_FILE "/dev/tuxedo_io"

class TuxedoIOAPI : public DeviceInterface
{
private:
  IO m_io = IO(TUXEDO_IO_DEVICE_FILE);
  std::unique_ptr<DeviceInterface> m_activeDevice;

  std::unique_ptr<DeviceInterface> detectAndCreateDevice()
  {
    bool identified = false;

    // Try Clevo device
    if ( ClevoDevice::canIdentify( m_io, identified ) and identified )
      return std::make_unique< ClevoDevice >( m_io );

    // Try Uniwill device
    if ( UniwillDevice::canIdentify( m_io, identified ) and identified )
      return std::make_unique< UniwillDevice >( m_io );

    // No device found, return dummy as fallback
    return std::make_unique< DummyDevice >( m_io );
  }

public:
  TuxedoIOAPI() : DeviceInterface(m_io),
    m_activeDevice( detectAndCreateDevice() )
  {
  }

  bool wmiAvailable()
  { return m_io.isAvailable(); }

  bool getModuleVersion(std::string &version)
  { return m_io.ioctlCall( R_MOD_VERSION, version, 20 ); }

  bool getModuleAPIMinVersion(std::string &version)
  { version = MOD_API_MIN_VERSION; return true;}

  virtual bool identify( bool &identified )
  { return m_activeDevice->identify( identified ); }

  virtual bool deviceInterfaceIdStr( std::string &interfaceIdStr )
  { return m_activeDevice->deviceInterfaceIdStr( interfaceIdStr ); }

  virtual bool deviceModelIdStr( std::string &modelIdStr )
  { return m_activeDevice->deviceModelIdStr( modelIdStr ); }

  virtual bool setEnableModeSet( bool enabled )
  { return m_activeDevice->setEnableModeSet( enabled ); }

  virtual bool getFansMinSpeed( int &minSpeed )
  { return m_activeDevice->getFansMinSpeed( minSpeed ); }

  virtual bool getFansOffAvailable( bool &offAvailable )
  { return m_activeDevice->getFansOffAvailable( offAvailable ); }

  virtual bool getNumberFans( int &nrFans )
  { return m_activeDevice->getNumberFans( nrFans ); }

  virtual bool setFansAuto()
  { return m_activeDevice->setFansAuto(); }

  virtual bool setFanSpeedPercent( const int fanNr, const int fanSpeedPercent )
  { return m_activeDevice->setFanSpeedPercent( fanNr, fanSpeedPercent ); }

  virtual bool getFanSpeedPercent( const int fanNr, int &fanSpeedPercent )
  { return m_activeDevice->getFanSpeedPercent( fanNr, fanSpeedPercent ); }

  virtual bool getFanTemperature( const int fanNr, int &temperatureCelcius )
  { return m_activeDevice->getFanTemperature( fanNr, temperatureCelcius ); }

  virtual bool setWebcam( const bool status )
  { return m_activeDevice->setWebcam( status ); }

  virtual bool getWebcam( bool &status )
  { return m_activeDevice->getWebcam( status ); }

  virtual bool getAvailableODMPerformanceProfiles( std::vector< std::string > &profiles )
  { return m_activeDevice->getAvailableODMPerformanceProfiles( profiles ); }

  virtual bool setODMPerformanceProfile( std::string performanceProfile )
  { return m_activeDevice->setODMPerformanceProfile( performanceProfile ); }

  virtual bool getDefaultODMPerformanceProfile( std::string &profileName )
  { return m_activeDevice->getDefaultODMPerformanceProfile( profileName ); }

  virtual bool getNumberTDPs( int &nrTDPs )
  { return m_activeDevice->getNumberTDPs( nrTDPs ); }

  virtual bool getTDPDescriptors( std::vector< std::string > &tdpDescriptors )
  { return m_activeDevice->getTDPDescriptors( tdpDescriptors ); }

  virtual bool getTDPMin( const int tdpIndex, int &minValue )
  { return m_activeDevice->getTDPMin( tdpIndex, minValue ); }

  virtual bool getTDPMax( const int tdpIndex, int &maxValue )
  { return m_activeDevice->getTDPMax( tdpIndex, maxValue ); }

  virtual bool setTDP( const int tdpIndex, int tdpValue )
  { return m_activeDevice->setTDP( tdpIndex, tdpValue ); }

  virtual bool getTDP( const int tdpIndex, int &tdpValue )
  { return m_activeDevice->getTDP( tdpIndex, tdpValue ); }
};
