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

#include "SysfsNode.hpp"
#include <string>
#include <filesystem>

namespace fs = std::filesystem;

/**
 * @brief Controller for Function Lock (FnLock)
 *
 * Manages the Fn key lock state through the tuxedo_keyboard kernel module.
 * When FnLock is enabled, function keys (F1-F12) act as media keys by default,
 * and Fn+Fkey gives the traditional F-key function.
 */
class FnLockController
{
public:
  FnLockController()
    : m_fnLockPath( "/sys/devices/platform/tuxedo_keyboard/fn_lock" )
  {
  }

  /**
   * @brief Check if FnLock is supported on this system
   * @return true if the sysfs node exists, false otherwise
   */
  bool isSupported() const
  {
    std::error_code ec;
    return fs::exists( m_fnLockPath, ec );
  }

  /**
   * @brief Get the current FnLock status
   * @return true if FnLock is enabled, false if disabled or unavailable
   */
  bool getStatus() const
  {
    if ( !isSupported() )
      return false;

    SysfsNode< bool > fnLock( m_fnLockPath );
    return fnLock.read().value_or( false );
  }

  /**
   * @brief Set the FnLock status
   * @param enabled true to enable FnLock, false to disable
   * @return true if successful, false otherwise
   */
  bool setStatus( bool enabled )
  {
    if ( !isSupported() )
      return false;

    SysfsNode< bool > fnLock( m_fnLockPath );
    return fnLock.write( enabled );
  }

private:
  std::string m_fnLockPath;
};
