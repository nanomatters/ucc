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

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <memory>
#include <string>

/**
 * @brief IO device interface for TUXEDO I/O operations
 *
 * Provides a wrapper around device file I/O operations with ioctl support.
 * Uses Qt6 for memory management and string handling with proper camelCase naming.
 */
class IO
{
public:
  /**
   * @brief Construct an IO device handle
   * @param devicePath Path to the device file to open
   */
  explicit IO(const std::string &devicePath)
    : m_fileHandle(-1)
  {
    openDevice(devicePath);
  }

  /**
   * @brief Destructor - closes the device handle
   */
  ~IO()
  {
    closeDevice();
  }

  /**
   * @brief Check if the device is available and open
   * @return true if device is successfully opened, false otherwise
   */
  bool isAvailable() const
  {
    return m_fileHandle >= 0;
  }

  /**
   * @brief Execute an ioctl call without arguments
   * @param request The ioctl request code
   * @return true if the operation succeeded, false otherwise
   */
  bool ioctlCall(unsigned long request)
  {
    if ( not isAvailable() )
      return false;

    int result = ioctl(m_fileHandle, request);
    return result >= 0;
  }

  /**
   * @brief Execute an ioctl call with an integer argument
   * @param request The ioctl request code
   * @param argument Reference to the integer argument for the ioctl call
   * @return true if the operation succeeded, false otherwise
   */
  bool ioctlCall(unsigned long request, int &argument)
  {
    if ( not isAvailable() )
      return false;

    int result = ioctl(m_fileHandle, request, &argument);
    return result >= 0;
  }

  /**
   * @brief Execute an ioctl call with a string/buffer argument
   * @param request The ioctl request code
   * @param argument Reference to the string where the result will be stored
   * @param bufferLength The size of the buffer to allocate
   * @return true if the operation succeeded, false otherwise
   */
  bool ioctlCall(unsigned long request, std::string &argument, size_t bufferLength)
  {
    if ( not isAvailable() )
      return false;

    auto buffer = std::make_unique<char[]>(bufferLength);
    int result = ioctl(m_fileHandle, request, buffer.get());
    if ( result >= 0 )
    {
      argument.clear();
      argument.append(buffer.get());
    }
    return result >= 0;
  }

private:
  int m_fileHandle;  ///< File descriptor for the device

  /**
   * @brief Open the device file
   * @param devicePath Path to the device file
   */
  void openDevice(const std::string &devicePath)
  {
    m_fileHandle = open(devicePath.c_str(), O_RDWR);
  }

  /**
   * @brief Close the device file
   */
  void closeDevice()
  {
    if ( m_fileHandle >= 0 )
    {
      close(m_fileHandle);
      m_fileHandle = -1;
    }
  }
};
