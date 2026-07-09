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

#include "workers/DisplayWorker.hpp"
#include "UniwillSysfs.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <ranges>
#include <cctype>
#include <dirent.h>
#include <unistd.h>
#include <utmpx.h>

namespace fs = std::filesystem;

// ============================================================================
// DisplayBacklightController
// ============================================================================

DisplayBacklightController::DisplayBacklightController( const std::string &basePath,
                                                        int maxBrightness,
                                                        bool isAmdgpuBl )
  : m_basePath( basePath ), m_maxBrightness( maxBrightness ), m_isAmdgpuBl( isAmdgpuBl )
{
  // Extract driver name from path
  m_driverName = fs::path( basePath ).filename().string();
}

int DisplayBacklightController::getBrightness() const noexcept
{
  SysfsNode< int > brightnessNode( m_basePath + "/brightness" );
  auto value = brightnessNode.read();

  if ( not value.has_value() )
    return -1;

  int rawBrightness = value.value();

  // amdgpu_bl has inverted brightness: 0 = max, max = off
  if ( m_isAmdgpuBl )
    rawBrightness = m_maxBrightness - rawBrightness;

  // Convert to percentage
  if ( m_maxBrightness <= 0 )
    return -1;

  return static_cast< int >( ( static_cast< double >( rawBrightness ) / m_maxBrightness ) * 100.0 );
}

bool DisplayBacklightController::setBrightness( int brightness ) noexcept
{
  if ( m_maxBrightness <= 0 )
    return false;

  // Convert percentage to raw value
  int rawBrightness = static_cast< int >( ( static_cast< double >( brightness ) / 100.0 ) * m_maxBrightness );
  rawBrightness = std::clamp( rawBrightness, 0, m_maxBrightness );

  // amdgpu_bl: 0 = max, max = off (inverted)
  if ( m_isAmdgpuBl )
    rawBrightness = m_maxBrightness - rawBrightness;

  SysfsNode< int > brightnessNode( m_basePath + "/brightness" );
  return brightnessNode.write( rawBrightness );
}

// ============================================================================
// DisplayWorker
// ============================================================================

DisplayWorker::DisplayWorker(
  const std::string &autosavePath,
  std::function< UccProfile() > getActiveProfile,
  std::function< int32_t() > getAutosaveBrightness,
  std::function< void( int32_t ) > setAutosaveBrightness,
  std::function< bool() > getIsX11Callback,
  std::function< void( const std::string & ) > setDisplayModesCallback,
  std::function< void( bool ) > setIsX11Callback )
  : DaemonWorker( std::chrono::milliseconds( 3000 ), false )
  , m_getActiveProfile( getActiveProfile )
  , m_autosavePath( autosavePath )
  , m_getAutosaveBrightness( getAutosaveBrightness )
  , m_setAutosaveBrightness( setAutosaveBrightness )
  , m_getIsX11( getIsX11Callback )
  , m_setDisplayModes( setDisplayModesCallback )
  , m_setIsX11( setIsX11Callback )
  , m_displayInfoFound( false )
  , m_previousUsers( "" )
  , m_isX11( false )
  , m_isWayland( false )
  , m_displayEnvVariable( "" )
  , m_xAuthorityFile( "" )
  , m_refreshRateCycleCounter( 0 )
{
}

// Public API

bool DisplayWorker::setBrightness( int32_t brightness ) noexcept
{
  if ( not m_backlightController )
    return false;

  return m_backlightController->setBrightness( brightness );
}

std::optional< DisplayMode > DisplayWorker::getActiveDisplayMode() noexcept
{
  if ( not m_displayInfoFound )
    updateDisplayData();

  if ( m_displayInfo.displayName.empty() )
    return std::nullopt;

  return m_displayInfo.activeMode;
}

bool DisplayWorker::setRefreshRate( int refreshRate ) noexcept
{
  if ( not m_displayInfoFound )
    updateDisplayData();

  if ( m_displayInfo.displayName.empty() or m_displayInfo.activeMode.xResolution == 0 )
    return false;

  setDisplayMode( m_displayInfo.activeMode.xResolution,
                  m_displayInfo.activeMode.yResolution,
                  refreshRate );
  return true;
}

// DaemonWorker lifecycle

void DisplayWorker::onStart()
{
  // Initialize backlight hardware
  initBacklight();
  applyBacklightFromProfile();

  // Refresh rate environment variables will be discovered on first onWork() call
}

void DisplayWorker::onWork()
{
  reenumerateBacklightDrivers();

  // Refresh rate work (every 2nd cycle about 6000ms, close to original 5000ms)
  m_refreshRateCycleCounter++;
  if ( m_refreshRateCycleCounter % 2 == 0 )
  {
    const auto [usersAvailable, usersChanged] = checkUsers();

    if ( usersChanged )
      resetRefreshRateState();

    if ( usersAvailable and not m_isWayland )
    {
      if ( not m_displayInfoFound )
        updateDisplayData();

      setActiveDisplayMode();
    }
  }
}

void DisplayWorker::onExit()
{
}

// ============================================================================
// Backlight implementation
// ============================================================================

void DisplayWorker::initBacklight()
{
  try
  {
    const fs::path backlightBase = "/sys/class/backlight";

    if ( not SysfsNode< std::string >::exists( backlightBase ) )
      return;

    for ( const fs::path &entry : SysfsNode< std::string >::directoryEntries( backlightBase ) )
    {
      std::string driverPath = entry.string();
      std::string maxBrightnessPath = ( entry / "max_brightness" ).string();

      if ( not SysfsNode< std::string >::exists( maxBrightnessPath ) )
        continue;

      SysfsNode< int > maxBrightness( maxBrightnessPath );
      auto maxVal = maxBrightness.read();
      if ( not maxVal.has_value() or maxVal.value() <= 0 )
        continue;

      std::string driverName = entry.filename().string();
      bool isAmdgpuBl = ( driverName == "amdgpu_bl0" or driverName == "amdgpu_bl1" );

      m_backlightController = std::make_unique< DisplayBacklightController >(
        driverPath, maxVal.value(), isAmdgpuBl );

      syslog( LOG_INFO, "DisplayWorker: Backlight driver '%s' (max=%d, amdgpu_bl=%d)",
              driverName.c_str(), maxVal.value(), isAmdgpuBl ? 1 : 0 );
      break; // Use first available driver
    }
  }
  catch ( const std::exception &e )
  {
    syslog( LOG_WARNING, "DisplayWorker: Failed to init backlight: %s", e.what() );
  }
}

void DisplayWorker::applyBacklightFromProfile()
{
  const UccProfile activeProfile = m_getActiveProfile();

  if ( m_backlightController and activeProfile.display.brightness >= 0 )
  {
    m_backlightController->setBrightness( activeProfile.display.brightness );
    syslog( LOG_INFO, "DisplayWorker: Applied profile brightness %d%%",
            activeProfile.display.brightness );
  }

  if ( activeProfile.display.miniLedLocalDimming.has_value() )
  {
    const auto control = ucc::uniwill::readMiniLedLocalDimmingControl();
    if ( control.isAvailable() )
    {
      const SysfsWriteResult result = ucc::uniwill::writeMiniLedLocalDimming(
        control,
        *activeProfile.display.miniLedLocalDimming );

      if ( result )
      {
        syslog( LOG_INFO, "DisplayWorker: Applied profile mini-LED local dimming %s",
                *activeProfile.display.miniLedLocalDimming ? "enabled" : "disabled" );
      }
      else
      {
        syslog( LOG_WARNING, "DisplayWorker: Failed to apply mini-LED local dimming (errno %d)",
                result.error );
      }
    }
  }
}

void DisplayWorker::reenumerateBacklightDrivers()
{
  // Re-check if backlight controller is still valid
  // If it was lost (e.g., driver reload), try to reinitialize
  if ( not m_backlightController )
  {
    initBacklight();
  }
}

// ============================================================================
// Refresh rate implementation
// ============================================================================

std::pair< bool, bool > DisplayWorker::checkUsers() noexcept
{
  // Use utmpx API instead of popen("users") to avoid shell invocation
  std::string loggedInUsers;

  try
  {
    setutxent();
    struct utmpx *entry;
    while ( ( entry = getutxent() ) != nullptr )
    {
      if ( entry->ut_type == USER_PROCESS )
      {
        if ( not loggedInUsers.empty() )
          loggedInUsers += ' ';
        loggedInUsers += entry->ut_user;
      }
    }
    endutxent();
  }
  catch ( ... )
  {
    // Ignore errors
  }

  const bool usersAvailable = not loggedInUsers.empty();
  const bool usersChanged = loggedInUsers != m_previousUsers;

  m_previousUsers = loggedInUsers;

  return { usersAvailable, usersChanged };
}

void DisplayWorker::resetRefreshRateState() noexcept
{
  m_displayInfo = DisplayInfo();
  m_displayInfoFound = false;
  m_isX11 = false;
  m_isWayland = false;
  m_displayEnvVariable = "";
  m_xAuthorityFile = "";
  m_displayName = "";
}

/**
 * @brief Validate that a string looks like a safe DISPLAY value.
 * Must match :N or :N.N where N is a small number.
 */
static bool isValidDisplay( const std::string &v ) noexcept
{
  static const std::regex pattern( R"(^:[0-9]{1,3}(\.[0-9]{1,3})?$)" );
  return std::regex_match( v, pattern );
}

/**
 * @brief Validate that a string looks like a safe XAUTHORITY path.
 * Only allow path characters [a-zA-Z0-9/_.-].
 */
static bool isValidXAuthority( const std::string &v ) noexcept
{
  static const std::regex pattern( R"(^/[a-zA-Z0-9/_. -]+$)" );
  return std::regex_match( v, pattern );
}

/**
 * @brief Read environment variables from /proc/<pid>/environ directly.
 *
 * Replaces the old popen()/shell pipeline that was vulnerable to command
 * injection via attacker-controlled environment variables.
 */
void DisplayWorker::setEnvVariables() noexcept
{
  try
  {
    std::string display, xauthority, sessionType;
    const uid_t rootUid = 0;

    // Iterate /proc to find non-root user processes
    for ( const auto &entry : fs::directory_iterator( "/proc" ) )
    {
      if ( not display.empty() and not xauthority.empty() and not sessionType.empty() )
        break;

      const std::string pidName = entry.path().filename().string();

      // Only numeric directory names (PIDs)
      if ( pidName.empty() or not std::isdigit( static_cast< unsigned char >( pidName[0] ) ) )
        continue;

      // Check process owner - skip root
      std::error_code ec;
      (void) fs::status( entry.path(), ec );  // probe accessibility
      if ( ec )
        continue;

      // Read /proc/<pid>/loginuid to determine the owning user
      std::string loginuidPath = entry.path().string() + "/loginuid";
      std::ifstream loginuidFile( loginuidPath );
      if ( loginuidFile )
      {
        uid_t loginuid = 0;
        loginuidFile >> loginuid;
        if ( loginuid == rootUid or loginuid == static_cast< uid_t >( -1 ) )
          continue;
      }
      else
      {
        continue;
      }

      // Read /proc/<pid>/environ (NUL-separated key=value pairs)
      std::string environPath = entry.path().string() + "/environ";
      std::ifstream envFile( environPath, std::ios::binary );
      if ( not envFile )
        continue;

      std::string envContent( ( std::istreambuf_iterator< char >( envFile ) ),
                               std::istreambuf_iterator< char >() );

      // Parse NUL-separated entries
      std::istringstream envStream( envContent );
      std::string envEntry;
      while ( std::getline( envStream, envEntry, '\0' ) )
      {
        if ( display.empty() and envEntry.starts_with( "DISPLAY=" ) )
          display = envEntry.substr( 8 );
        else if ( xauthority.empty() and envEntry.starts_with( "XAUTHORITY=" ) )
          xauthority = envEntry.substr( 11 );
        else if ( sessionType.empty() and envEntry.starts_with( "XDG_SESSION_TYPE=" ) )
          sessionType = envEntry.substr( 17 );
      }
    }

    // Skip login screen sessions
    if ( xauthority.find( "/var/run/sddm/{" ) != std::string::npos or
         xauthority.find( "/var/lib/lightdm" ) != std::string::npos )
      return;

    // Trim whitespace from session type
    while ( not sessionType.empty() and
            ( sessionType.back() == '\n' or sessionType.back() == '\r' or sessionType.back() == ' ' ) )
      sessionType.pop_back();

    // Convert to lowercase
    for ( char &c : sessionType )
      c = static_cast< char >( std::tolower( static_cast< unsigned char >( c ) ) );

    // INPUT VALIDATION: reject values that contain shell metacharacters
    if ( not display.empty() and not isValidDisplay( display ) )
    {
      syslog( LOG_WARNING, "DisplayWorker: Rejected suspicious DISPLAY value" );
      display.clear();
    }
    if ( not xauthority.empty() and not isValidXAuthority( xauthority ) )
    {
      syslog( LOG_WARNING, "DisplayWorker: Rejected suspicious XAUTHORITY value" );
      xauthority.clear();
    }

    m_displayEnvVariable = display;
    m_xAuthorityFile = xauthority;
    m_isX11 = ( sessionType == "x11" );
    m_isWayland = ( sessionType == "wayland" );
  }
  catch ( ... )
  {
    // Reset on error
    resetRefreshRateState();
  }
}

std::optional< DisplayInfo > DisplayWorker::getDisplayModes() noexcept
{
  if ( m_displayEnvVariable.empty() or m_xAuthorityFile.empty() )
    setEnvVariables();

  if ( m_displayEnvVariable.empty() or m_xAuthorityFile.empty() or m_isWayland )
    return std::nullopt;

  try
  {
    // Use executeProcess() with argument array instead of popen() shell
    // to prevent command injection via m_xAuthorityFile / m_displayEnvVariable
    std::string result = ucc::executeProcess(
        "xrandr",
        { "-q", "-display", m_displayEnvVariable, "--current" },
        { "XAUTHORITY=" + m_xAuthorityFile } );

    if ( result.empty() )
      return std::nullopt;

    return parseXrandrOutput( result );
  }
  catch ( ... )
  {
    return std::nullopt;
  }
}

std::optional< DisplayInfo > DisplayWorker::parseXrandrOutput( const std::string &output ) noexcept
{
  try
  {
    DisplayInfo info;

    // Find display name (eDP* or LVDS*)
    std::regex displayNameRegex( R"((eDP\S*|LVDS\S*))" );
    std::smatch displayMatch;
    if ( std::regex_search( output, displayMatch, displayNameRegex ) )
    {
      info.displayName = m_displayName = displayMatch[1].str();
    }
    else
    {
      return std::nullopt;
    }

    // Parse resolution and refresh rate lines
    std::regex lineRegex( R"(\s+([0-9]{3,4})x([0-9]{3,4})[a-z]?(\s+[0-9]{1,3}\.[0-9]{2}[\*]?[\+]?)+)" );
    std::regex rateRegex( R"([0-9]{1,3}\.[0-9]{2}[\*]?[\+]?)" );

    std::istringstream iss( output );
    std::string line;
    bool foundDisplayName = false;

    while ( std::getline( iss, line ) )
    {
      if ( not foundDisplayName and line.find( info.displayName ) != std::string::npos )
      {
        foundDisplayName = true;
        continue;
      }

      if ( foundDisplayName )
      {
        std::smatch lineMatch;
        if ( std::regex_search( line, lineMatch, lineRegex ) )
        {
          DisplayMode mode;
          mode.xResolution = std::stoi( lineMatch[1].str() );
          mode.yResolution = std::stoi( lineMatch[2].str() );

          // Find all refresh rates
          std::string ratesPart = lineMatch[0].str();
          std::sregex_iterator rateIter( ratesPart.begin(), ratesPart.end(), rateRegex );
          std::sregex_iterator rateEnd;

          int activeRateIndex = -1;
          int idx = 0;

          for ( ; rateIter != rateEnd; ++rateIter, ++idx )
          {
            std::string rateStr = ( *rateIter )[0].str();

            // Check if this is the active rate (marked with *)
            if ( rateStr.find( '*' ) != std::string::npos )
              activeRateIndex = idx;

            // Remove * and + markers
            rateStr.erase( std::remove_if( rateStr.begin(), rateStr.end(),
              []( char c ) { return c == '*' or c == '+'; } ), rateStr.end() );

            double rate = std::stod( rateStr );

            // Avoid duplicates
            if ( std::ranges::find( mode.refreshRates, rate ) == mode.refreshRates.end() )
              mode.refreshRates.push_back( rate );
          }

          info.displayModes.push_back( mode );

          // Set active mode
          if ( activeRateIndex >= 0 and static_cast< size_t >( activeRateIndex ) < mode.refreshRates.size() )
          {
            info.activeMode.xResolution = mode.xResolution;
            info.activeMode.yResolution = mode.yResolution;
            info.activeMode.refreshRates = { mode.refreshRates[static_cast< size_t >( activeRateIndex )] };
          }
        }
      }
    }

    return info;
  }
  catch ( ... )
  {
    return std::nullopt;
  }
}

void DisplayWorker::updateDisplayData() noexcept
{
  auto modes = getDisplayModes();

  m_setIsX11( m_isX11 );

  if ( not modes )
  {
    m_setDisplayModes( "" );
    m_displayInfoFound = false;
  }
  else
  {
    m_displayInfo = *modes;
    m_displayInfoFound = true;
    m_setDisplayModes( serializeDisplayInfo( *modes ) );
  }
}

std::string DisplayWorker::serializeDisplayInfo( const DisplayInfo &info ) const noexcept
{
  std::ostringstream oss;

  oss << "{";
  oss << "\"displayName\":\"" << info.displayName << "\",";

  // Active mode
  oss << "\"activeMode\":{";
  oss << "\"xResolution\":" << info.activeMode.xResolution << ",";
  oss << "\"yResolution\":" << info.activeMode.yResolution << ",";
  oss << "\"refreshRates\":[";
  for ( size_t i = 0; i < info.activeMode.refreshRates.size(); ++i )
  {
    if ( i > 0 ) oss << ",";
    oss << info.activeMode.refreshRates[i];
  }
  oss << "]},";

  // Display modes
  oss << "\"displayModes\":[";
  for ( size_t i = 0; i < info.displayModes.size(); ++i )
  {
    if ( i > 0 ) oss << ",";
    const auto &mode = info.displayModes[i];
    oss << "{";
    oss << "\"xResolution\":" << mode.xResolution << ",";
    oss << "\"yResolution\":" << mode.yResolution << ",";
    oss << "\"refreshRates\":[";
    for ( size_t j = 0; j < mode.refreshRates.size(); ++j )
    {
      if ( j > 0 ) oss << ",";
      oss << mode.refreshRates[j];
    }
    oss << "]}";
  }
  oss << "]}";

  return oss.str();
}

void DisplayWorker::setActiveDisplayMode() noexcept
{
  const UccProfile activeProfile = m_getActiveProfile();

  if ( not activeProfile.display.useRefRate )
    return;

  if ( m_displayInfo.activeMode.refreshRates.empty() )
    return;

  const double currentRefreshRate = m_displayInfo.activeMode.refreshRates[0];
  const int32_t desiredRefreshRate = activeProfile.display.refreshRate;

  if ( desiredRefreshRate <= 0 )
    return;

  if ( std::abs( currentRefreshRate - static_cast< double >( desiredRefreshRate ) ) < 0.1 )
    return; // Already at desired refresh rate

  setDisplayMode( m_displayInfo.activeMode.xResolution,
                  m_displayInfo.activeMode.yResolution,
                  desiredRefreshRate );

  // Update cached value
  m_displayInfo.activeMode.refreshRates[0] = static_cast< double >( desiredRefreshRate );
}

void DisplayWorker::setDisplayMode( int xRes, int yRes, int refRate ) noexcept
{
  if ( not m_isX11 or m_displayEnvVariable.empty() or m_xAuthorityFile.empty() or m_displayName.empty() )
    return;

  try
  {
    // Use executeProcess() with argument array instead of popen() shell
    // to prevent command injection via m_xAuthorityFile / m_displayEnvVariable / m_displayName
    std::string modeStr = std::to_string( xRes ) + "x" + std::to_string( yRes );
    (void) ucc::executeProcess(
        "xrandr",
        { "-display", m_displayEnvVariable,
          "--output", m_displayName,
          "--mode", modeStr,
          "-r", std::to_string( refRate ) },
        { "XAUTHORITY=" + m_xAuthorityFile } );

    syslog( LOG_INFO, "DisplayWorker: Set display mode to %dx%d @ %dHz", xRes, yRes, refRate );
  }
  catch ( ... )
  {
    syslog( LOG_WARNING, "DisplayWorker: Failed to set display mode" );
  }
}

void DisplayWorker::reapplyProfile() noexcept
{
  applyBacklightFromProfile();
}
