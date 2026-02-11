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

#include "DaemonWorker.hpp"
#include "../SysfsNode.hpp"
#include "../Utils.hpp"
#include "../profiles/UccProfile.hpp"
#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <cstdio>
#include <memory>
#include <regex>
#include <syslog.h>

/**
 * @brief Display mode structure
 */
struct DisplayMode
{
  std::vector< double > refreshRates;
  int xResolution;
  int yResolution;

  DisplayMode() : xResolution( 0 ), yResolution( 0 ) {}
};

/**
 * @brief Display information structure
 */
struct DisplayInfo
{
  std::string displayName;
  DisplayMode activeMode;
  std::vector< DisplayMode > displayModes;

  DisplayInfo() : displayName( "" ) {}
};

/**
 * @brief Controller for display backlight brightness via sysfs
 *
 * Reads and writes brightness through sysfs LED interface.
 * Includes workaround for amdgpu_bl driver brightness inversion.
 */
class DisplayBacklightController
{
public:
  explicit DisplayBacklightController( const std::string &basePath, int maxBrightness, bool isAmdgpuBl );

  [[nodiscard]] int getBrightness() const noexcept;
  bool setBrightness( int brightness ) noexcept;
  [[nodiscard]] const std::string &getDriverName() const noexcept { return m_driverName; }

private:
  std::string m_basePath;
  std::string m_driverName;
  int m_maxBrightness;
  bool m_isAmdgpuBl;
};

/**
 * @brief DisplayWorker - Unified display hardware management
 *
 * Combines display backlight brightness control and display refresh rate management
 * into a single worker thread.
 *
 * Backlight features:
 *   - Detects backlight hardware via sysfs (/sys/class/backlight)
 *   - Applies brightness from active profile on start
 *   - Periodically persists brightness to autosave
 *   - Works with Intel, AMD, and amdgpu_bl backlight drivers
 *
 * Refresh rate features (X11 only):
 *   - Detects available display modes and refresh rates via xrandr
 *   - Applies refresh rate from active profile
 *   - Only active on X11 sessions (disabled on Wayland)
 *   - Monitors user login/logout to reset state
 */
class DisplayWorker : public DaemonWorker
{
public:
  DisplayWorker(
    const std::string &autosavePath,
    std::function< UccProfile() > getActiveProfile,
    std::function< int32_t() > getAutosaveBrightness,
    std::function< void( int32_t ) > setAutosaveBrightness,
    std::function< bool() > getIsX11Callback,
    std::function< void( const std::string & ) > setDisplayModesCallback,
    std::function< void( bool ) > setIsX11Callback );

  ~DisplayWorker() override = default;

  // Prevent copy and move
  DisplayWorker( const DisplayWorker & ) = delete;
  DisplayWorker( DisplayWorker && ) = delete;
  DisplayWorker &operator=( const DisplayWorker & ) = delete;
  DisplayWorker &operator=( DisplayWorker && ) = delete;

  /**
   * @brief Set display backlight brightness
   * @param brightness Brightness percentage (0-100)
   * @return true if successful
   */
  bool setBrightness( int32_t brightness ) noexcept;

  /**
   * @brief Get current active display mode
   * @return Active display mode or nullopt if not available
   */
  std::optional< DisplayMode > getActiveDisplayMode() noexcept;

  /**
   * @brief Set refresh rate for the active display
   * @param refreshRate The refresh rate to set (in Hz)
   * @return true if successful
   */
  bool setRefreshRate( int refreshRate ) noexcept;

protected:
  void onStart() override;
  void onWork() override;
  void onExit() override;

private:
  // --- Shared state ---
  std::function< UccProfile() > m_getActiveProfile;

  // --- Backlight state ---
  std::string m_autosavePath;
  std::function< int32_t() > m_getAutosaveBrightness;
  std::function< void( int32_t ) > m_setAutosaveBrightness;
  std::unique_ptr< DisplayBacklightController > m_backlightController;

  void initBacklight();
  void applyBacklightFromProfile();
  void persistBrightness();
  void reenumerateBacklightDrivers();

  // --- Refresh rate state ---
  std::function< bool() > m_getIsX11;
  std::function< void( const std::string & ) > m_setDisplayModes;
  std::function< void( bool ) > m_setIsX11;

  DisplayInfo m_displayInfo;
  bool m_displayInfoFound;
  std::string m_previousUsers;
  bool m_isX11;
  bool m_isWayland;
  std::string m_displayEnvVariable;
  std::string m_xAuthorityFile;
  std::string m_displayName;
  uint32_t m_refreshRateCycleCounter;

  std::pair< bool, bool > checkUsers() noexcept;
  void resetRefreshRateState() noexcept;
  void setEnvVariables() noexcept;
  std::optional< DisplayInfo > getDisplayModes() noexcept;
  std::optional< DisplayInfo > parseXrandrOutput( const std::string &output ) noexcept;
  void updateDisplayData() noexcept;
  std::string serializeDisplayInfo( const DisplayInfo &info ) const noexcept;
  void setActiveDisplayMode() noexcept;
  void setDisplayMode( int xRes, int yRes, int refRate ) noexcept;
};
