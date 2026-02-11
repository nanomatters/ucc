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
#include <string>
#include <cmath>
#include <libudev.h>
#include <vector>
#include "tuxedo_io_lib/tuxedo_io_api.hh"

using namespace Napi;

Boolean GetModuleInfo(const CallbackInfo &info)
{
  TuxedoIOAPI io;
  if ( info.Length() != 1 or not info[0].IsObject() )
  {
    throw Napi::Error::New(info.Env(), "GetModuleInfo - invalid argument");
  }

  Object moduleInfo = info[0].As<Object>();
  std::string version;

  bool result = io.getModuleVersion( version );
  moduleInfo.Set( "version", version );

  std::string activeInterface;
  if ( io.deviceInterfaceIdStr( activeInterface ) )
  {
    moduleInfo.Set( "activeInterface", activeInterface );
  }
  else
  {
    moduleInfo.Set( "activeInterface", "inactive" );
  }

  std::string deviceIdStr;
  io.deviceModelIdStr( deviceIdStr );
  moduleInfo.Set( "model", deviceIdStr );

  return Boolean::New(info.Env(), result);
}

static inline bool CheckMinVersionByStrings(std::string version, std::string minVersion)
{
  unsigned modVersionMajor, modVersionMinor, modVersionPatch, modAPIMinVersionMajor, modAPIMinVersionMinor, modAPIMinVersionPatch;
  if ( sscanf(version.c_str(), "%u.%u.%u", &modVersionMajor, &modVersionMinor, &modVersionPatch) < 3 or
       sscanf(minVersion.c_str(), "%u.%u.%u", &modAPIMinVersionMajor, &modAPIMinVersionMinor, &modAPIMinVersionPatch) < 3 )
  {
    return false;
  }

  if ( modVersionMajor < modAPIMinVersionMajor or
       ( modVersionMajor == modAPIMinVersionMajor and modVersionMinor < modAPIMinVersionMinor ) or
       ( modVersionMajor == modAPIMinVersionMajor and modVersionMinor == modAPIMinVersionMinor and modVersionPatch < modAPIMinVersionPatch ) )
  {
    return false;
  }

  return true;
}

Boolean WmiAvailable(const CallbackInfo &info)
{
  TuxedoIOAPI io;

  std::string modVersion, modAPIMinVersion;

  bool availability = io.getModuleVersion( modVersion ) and
                      io.getModuleAPIMinVersion( modAPIMinVersion ) and
                      CheckMinVersionByStrings( modVersion, modAPIMinVersion ) and
                      io.wmiAvailable();

  return Boolean::New( info.Env(), availability );
}

Boolean SetEnableModeSet(const CallbackInfo &info)
{
  if ( info.Length() != 1 or not info[0].IsBoolean() )
  {
    throw Napi::Error::New(info.Env(), "SetEnableModeSet - invalid argument");
  }

  TuxedoIOAPI io;
  bool enabled = info[0].As<Boolean>();
  bool result = io.setEnableModeSet( enabled );
  return Boolean::New( info.Env(), result );
}

Number GetFansMinSpeed( const CallbackInfo &info )
{
  TuxedoIOAPI io;
  int minSpeed = 0;
  io.getFansMinSpeed( minSpeed );
  return Number::New( info.Env(), minSpeed );
}

Boolean GetFansOffAvailable( const CallbackInfo &info )
{
  TuxedoIOAPI io;
  bool offAvailable = true;
  io.getFansOffAvailable( offAvailable );
  return Boolean::New( info.Env(), offAvailable );
}

Number GetNumberFans( const CallbackInfo &info )
{
  TuxedoIOAPI io;
  int nrFans = 0;
  io.getNumberFans( nrFans );
  return Number::New( info.Env(), nrFans );
}

Boolean SetFansAuto( const CallbackInfo &info )
{
  TuxedoIOAPI io;
  bool result = io.setFansAuto();
  return Boolean::New( info.Env(), result );
}

Boolean SetFanSpeedPercent( const CallbackInfo &info )
{
  if ( info.Length() != 2 or not info[0].IsNumber() or not info[1].IsNumber() )
  {
    throw Napi::Error::New( info.Env(), "SetFanSpeedPercent - invalid argument" );
  }

  TuxedoIOAPI io;

  int fanNumber = info[0].As<Number>();
  int fanSpeedPercent = info[1].As<Number>();
  bool result = io.setFanSpeedPercent( fanNumber, fanSpeedPercent );
  return Boolean::New( info.Env(), result );
}

Boolean GetFanSpeedPercent( const CallbackInfo &info )
{
  if ( info.Length() != 2 or not info[0].IsNumber() or not info[1].IsObject() )
  {
    throw Napi::Error::New( info.Env(), "GetFanSpeedPercent - invalid argument" );
  }

  TuxedoIOAPI io;
  int fanNumber = info[0].As<Number>();
  int fanSpeedPercent;
  bool result = io.getFanSpeedPercent( fanNumber, fanSpeedPercent );
  Object objWrapper = info[1].As<Object>();
  objWrapper.Set( "value", fanSpeedPercent );
  return Boolean::New( info.Env(), result );
}

Boolean GetFanTemperature( const CallbackInfo &info )
{
  if ( info.Length() != 2 or not info[0].IsNumber() or not info[1].IsObject() )
  {
    throw Napi::Error::New( info.Env(), "GetFanTemperature - invalid argument" );
  }

  TuxedoIOAPI io;
  int fanNumber = info[0].As<Number>();
  int temperatureCelcius;
  bool result = io.getFanTemperature( fanNumber, temperatureCelcius );
  Object objWrapper = info[1].As<Object>();
  objWrapper.Set( "value", temperatureCelcius );
  return Boolean::New( info.Env(), result );
}

Boolean SetWebcamStatus( const CallbackInfo &info )
{
  TuxedoIOAPI io;
  if ( info.Length() != 1 or not info[0].IsBoolean() )
  {
    throw Napi::Error::New( info.Env(), "SetWebcamStatus - invalid argument" );
  }

  bool status = info[0].As<Boolean>();
  bool result = io.setWebcam( status );
  return Boolean::New( info.Env(), result );
}

Boolean GetWebcamStatus( const CallbackInfo &info )
{
  if ( info.Length() != 1 or not info[0].IsObject() )
  {
    throw Napi::Error::New( info.Env(), "GetWebcamStatus - invalid argument" );
  }

  TuxedoIOAPI io;
  bool status = false;
  bool result = io.getWebcam( status );
  Object objWrapper = info[0].As<Object>();
  objWrapper.Set( "value", status );
  return Boolean::New( info.Env(), result );
}

Array GetOutputPorts(const CallbackInfo &info)
{
  Array result;

  struct udev *udev_context = udev_new();
  if ( not udev_context )
  {
    // Placeholder for error log output
  }
  else
  {
    struct udev_enumerate *drm_devices = udev_enumerate_new(udev_context);
    if ( not drm_devices )
    {
      // Placeholder for error log output
    }
    else
    {
      struct udev_list_entry *drm_devices_iterator, *drm_devices_entry;
      if ( udev_enumerate_add_match_subsystem(drm_devices, "drm") < 0 or
           udev_enumerate_add_match_sysname(drm_devices, "card*-*-*") < 0 or
           udev_enumerate_scan_devices(drm_devices) < 0 or
           ( drm_devices_iterator = udev_enumerate_get_list_entry(drm_devices) ) == NULL )
      {
        // Placeholder for error log output
      }
      else
      {
        result = Array::New(info.Env());

        udev_list_entry_foreach(drm_devices_entry, drm_devices_iterator)
        {
          std::string path = udev_list_entry_get_name(drm_devices_entry);
          std::string name = path.substr(path.rfind("/") + 1);
          int card_number = std::stoi(name.substr(4, name.find("-") - 4));
          if ( not result.Has(card_number) )
          {
            result[card_number] = Array::New(info.Env());
          }

          result.Get(card_number).As<Array>()[result.Get(card_number).As<Array>().Length()] = name.substr(name.find("-") + 1);
        }
      }

      udev_enumerate_unref(drm_devices);
    }

    udev_unref(udev_context);
  }

  return result;
}

Boolean GetAvailableODMPerformanceProfiles( const CallbackInfo &info )
{
  if ( info.Length() != 1 or not info[0].IsObject() )
  {
    throw Napi::Error::New( info.Env(), "GetAvailableODMPerformanceProfiles - invalid argument" );
  }

  TuxedoIOAPI io;
  Object objWrapper = info[0].As<Object>();
  std::vector< std::string > profiles;
  bool result = io.getAvailableODMPerformanceProfiles( profiles );
  auto arr = Array::New( info.Env() );
  for ( std::size_t i = 0; i < profiles.size(); ++i )
  {
    arr.Set( i, profiles[ i ] );
  }

  objWrapper.Set( "value", arr );
  return Boolean::New( info.Env(), result );
}

Boolean SetODMPerformanceProfile( const CallbackInfo &info )
{
  if ( info.Length() != 1 or not info[0].IsString() )
  {
    throw Napi::Error::New( info.Env(), "SetODMPerformanceProfile - invalid argument" );
  }

  std::string performanceProfile = info[0].As<String>();
  TuxedoIOAPI io;
  bool result = io.setODMPerformanceProfile( performanceProfile );
  return Boolean::New( info.Env(), result );
}

Boolean GetDefaultODMPerformanceProfile( const CallbackInfo &info )
{
  if ( info.Length() != 1 or not info[0].IsObject() )
  {
    throw Napi::Error::New( info.Env(), "GetDefaultODMPerformanceProfile - invalid argument" );
  }

  Object objWrapper = info[0].As<Object>();
  TuxedoIOAPI io;
  std::string profileName;
  bool result = io.getDefaultODMPerformanceProfile( profileName );
  objWrapper.Set( "value", profileName );
  return Boolean::New( info.Env(), result );
}

Boolean GetTDPInfo( const CallbackInfo &info )
{
  if ( info.Length() != 1 or not info[0].IsArray() )
  {
    throw Napi::Error::New( info.Env(), "GetTDPInfo - invalid argument" );
  }

  Array tdpArray = info[0].As<Array>();
  TuxedoIOAPI io;
  bool result;
  int nrTDPs = 0;
  std::vector<std::string> tdpDescriptors;
  io.getTDPDescriptors( tdpDescriptors );
  result = io.getNumberTDPs( nrTDPs );
  for ( int i = 0; i < nrTDPs; ++i )
  {
    Object tdpInfo = Object::New( info.Env() );
    int minValue, maxValue, currentValue;
    io.getTDPMin( i, minValue );
    io.getTDPMax( i, maxValue );
    io.getTDP( i, currentValue );
    tdpInfo.Set( "min", minValue );
    tdpInfo.Set( "max", maxValue );
    tdpInfo.Set( "current", currentValue );
    tdpInfo.Set( "descriptor", tdpDescriptors.at( i ) );
    tdpArray[i] = tdpInfo;
  }

  return Boolean::New( info.Env(), result );
}

Boolean SetTDPValues( const CallbackInfo &info )
{
  if ( info.Length() != 1 or not info[0].IsArray() )
  {
    throw Napi::Error::New( info.Env(), "SetTDP - invalid argument" );
  }

  TuxedoIOAPI io;
  Array tdpValues = info[0].As<Array>();
  int nrInputs = tdpValues.Length();
  bool result;
  int nrTDPs = 0;
  result = io.getNumberTDPs( nrTDPs );
  for ( int i = 0; i < nrTDPs and i < nrInputs; ++i )
  {
    int32_t tdpValue;
    napi_status apiStatus = napi_get_value_int32( info.Env(), tdpValues.Get( i ), &tdpValue );
    if ( apiStatus != napi_ok )
    {
      throw Napi::Error::New( info.Env(), "SetTDP - invalid array element type" );
    }

    io.setTDP( i, tdpValue );
  }

  return Boolean::New( info.Env(), result );
}
