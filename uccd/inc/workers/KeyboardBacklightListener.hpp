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
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <chrono>

using namespace std::chrono_literals;
namespace fs = std::filesystem;

/**
 * @brief Keyboard backlight capabilities
 */
struct KeyboardBacklightCapabilities
{
  int zones = 0;
  int maxBrightness = 0;
  int maxRed = 0;
  int maxGreen = 0;
  int maxBlue = 0;
};

/**
 * @brief Single zone keyboard backlight state
 */
struct KeyboardBacklightState
{
  int brightness = 0;
  int red = 0;
  int green = 0;
  int blue = 0;
};

/**
 * @brief Listener for keyboard backlight changes and control
 *
 * Manages keyboard backlight hardware through sysfs LED interface.
 * Supports:
 * - White-only backlights
 * - RGB zone backlights (1-3 zones)
 * - Per-key RGB backlights
 */
class KeyboardBacklightListener : public DaemonWorker
{
public:
  KeyboardBacklightListener( std::function< void( const std::string & ) > setCapabilitiesJSON,
                            std::function< void( const std::string & ) > setStatesJSON,
                            std::function< std::string() > getNewStatesJSON,
                            std::function< void( const std::string & ) > clearNewStatesJSON,
                            std::function< bool() > getControlEnabled )
    : DaemonWorker( 2000ms )  // Check every 2 seconds
    , m_setCapabilitiesJSON( setCapabilitiesJSON )
    , m_setStatesJSON( setStatesJSON )
    , m_getNewStatesJSON( getNewStatesJSON )
    , m_clearNewStatesJSON( clearNewStatesJSON )
    , m_getControlEnabled( getControlEnabled )
  {
  }

  virtual ~KeyboardBacklightListener() = default;

  void onStart() override
  {
    detectKeyboardBacklight();

    if ( m_capabilities.zones > 0 )
    {
      std::cout << "[KeyboardBacklight] Detected " << m_capabilities.zones 
                << " zone(s), max brightness: " << m_capabilities.maxBrightness << std::endl;

      // Publish capabilities as JSON
      m_setCapabilitiesJSON( capabilitiesToJSON() );

      // Initialize default states with max brightness
      std::vector< KeyboardBacklightState > defaultStates;
      for ( int i = 0; i < m_capabilities.zones; ++i )
      {
        KeyboardBacklightState state;
        state.brightness = m_capabilities.maxBrightness;
        state.red = m_capabilities.maxRed > 0 ? m_capabilities.maxRed : 0;
        state.green = m_capabilities.maxGreen > 0 ? m_capabilities.maxGreen : 0;
        state.blue = m_capabilities.maxBlue > 0 ? m_capabilities.maxBlue : 0;
        defaultStates.push_back( state );
      }

      // Build initial states JSON
      std::string initialStatesJSON = statesToJSON( defaultStates );
      m_setStatesJSON( initialStatesJSON );
      
      // Apply if control is enabled
      if ( m_getControlEnabled() )
      {
        std::cout << "[KeyboardBacklight] Applying initial states (brightness: " 
                  << m_capabilities.maxBrightness << ")" << std::endl;
        applyStates( defaultStates );
        m_currentStates = defaultStates;
      }
    }
    else
    {
      std::cout << "[KeyboardBacklight] No keyboard backlight detected" << std::endl;
      m_setCapabilitiesJSON( "null" );
    }
  }

  void onWork() override
  {
    if ( m_capabilities.zones == 0 )
      return;

    // Check for new states from DBus
    std::string newStatesJSON = m_getNewStatesJSON();
    if ( !newStatesJSON.empty() )
    {
      m_clearNewStatesJSON( newStatesJSON );
      applyStatesFromJSON( newStatesJSON );
    }
  }

  void onExit() override
  {
    // No cleanup needed
  }

  void onActiveProfileChanged()
  {
    // Keyboard backlight is not profile-dependent currently
    // States are global and stored in settings
  }

  /**
   * @brief Apply keyboard backlight states from a profile
   * @param keyboardDataJSON JSON string containing keyboard backlight data from profile
   */
  void applyProfileKeyboardStates( const std::string &keyboardDataJSON )
  {
    if ( m_capabilities.zones == 0 || !m_getControlEnabled() )
      return;

    try
    {
      // Parse the keyboard data JSON
      std::string statesJSON = extractStatesFromKeyboardJSON( keyboardDataJSON );
      if ( !statesJSON.empty() )
      {
        applyStatesFromJSON( statesJSON );
      }
    }
    catch ( const std::exception &e )
    {
      std::cerr << "[KeyboardBacklight] Failed to apply profile keyboard states: " << e.what() << std::endl;
    }
  }

private:
  std::function< void( const std::string & ) > m_setCapabilitiesJSON;
  std::function< void( const std::string & ) > m_setStatesJSON;
  std::function< std::string() > m_getNewStatesJSON;
  std::function< void( const std::string & ) > m_clearNewStatesJSON;
  std::function< bool() > m_getControlEnabled;

  KeyboardBacklightCapabilities m_capabilities;
  std::vector< KeyboardBacklightState > m_currentStates;
  std::vector< std::string > m_ledPaths;

  // Common LED paths
  static constexpr const char *LEDS_WHITE_ONLY = "/sys/devices/platform/tuxedo_keyboard/leds/white:kbd_backlight";
  static constexpr const char *LEDS_WHITE_ONLY_NB05 = "/sys/bus/platform/devices/tuxedo_nb05_kbd_backlight/leds/white:kbd_backlight";
  static constexpr const char *LEDS_RGB_BASE = "/sys/devices/platform/tuxedo_keyboard/leds/rgb:kbd_backlight";

  void detectKeyboardBacklight()
  {
    m_capabilities = KeyboardBacklightCapabilities();

    // Check for white-only backlight
    if ( checkWhiteBacklight( LEDS_WHITE_ONLY ) )
    {
      std::cout << "[KeyboardBacklight] Detected white-only keyboard backlight" << std::endl;
      return;
    }

    if ( checkWhiteBacklight( LEDS_WHITE_ONLY_NB05 ) )
    {
      std::cout << "[KeyboardBacklight] Detected white-only keyboard backlight (NB05)" << std::endl;
      return;
    }

    // Check for RGB backlights
    detectRGBBacklight();
  }

  bool checkWhiteBacklight( const std::string &basePath )
  {
    std::string maxBrightnessPath = basePath + "/max_brightness";
    std::error_code ec;

    if ( !fs::exists( maxBrightnessPath, ec ) )
      return false;

    SysfsNode< int > maxBrightness( maxBrightnessPath );
    auto value = maxBrightness.read();
    if ( !value.has_value() )
      return false;

    m_capabilities.zones = 1;
    m_capabilities.maxBrightness = value.value();
    m_ledPaths.push_back( basePath );
    return true;
  }

  void detectRGBBacklight()
  {
    // First try to find per-key RGB keyboards
    findPerKeyRGBLEDs();

    // If we found per-key LEDs, we're done
    if ( m_ledPaths.size() > 3 )
    {
      std::cout << "[KeyboardBacklight] Detected per-key RGB keyboard with " 
                << m_ledPaths.size() << " zones" << std::endl;
      
      SysfsNode< int > maxBrightness( m_ledPaths[0] + "/max_brightness" );
      auto value = maxBrightness.read();
      
      m_capabilities.zones = static_cast< int >( m_ledPaths.size() );
      m_capabilities.maxBrightness = value.value_or( 255 );
      m_capabilities.maxRed = 0xFF;
      m_capabilities.maxGreen = 0xFF;
      m_capabilities.maxBlue = 0xFF;
      return;
    }

    // Otherwise, check for standard 1-3 zone RGB
    std::vector< std::string > rgbPaths = {
      LEDS_RGB_BASE,
      std::string( LEDS_RGB_BASE ) + "_1",
      std::string( LEDS_RGB_BASE ) + "_2"
    };

    std::error_code ec;
    for ( const auto &path : rgbPaths )
    {
      if ( fs::exists( path + "/max_brightness", ec ) )
      {
        m_ledPaths.push_back( path );
      }
    }

    if ( !m_ledPaths.empty() )
    {
      SysfsNode< int > maxBrightness( m_ledPaths[0] + "/max_brightness" );
      auto value = maxBrightness.read();

      m_capabilities.zones = static_cast< int >( m_ledPaths.size() );
      m_capabilities.maxBrightness = value.value_or( 255 );
      m_capabilities.maxRed = 0xFF;
      m_capabilities.maxGreen = 0xFF;
      m_capabilities.maxBlue = 0xFF;

      std::cout << "[KeyboardBacklight] Detected " << m_capabilities.zones 
                << " zone RGB keyboard backlight" << std::endl;
    }
  }

  void findPerKeyRGBLEDs()
  {
    std::vector< std::string > searchPaths = {
      "/sys/bus/hid/drivers/tuxedo-keyboard-ite",
      "/sys/bus/hid/drivers/ite_829x",
      "/sys/bus/hid/drivers/ite_8291",
      "/sys/bus/platform/drivers/tuxedo_nb04_kbd_backlight"
    };

    for ( const auto &driverPath : searchPaths )
    {
      std::error_code ec;
      if ( !fs::exists( driverPath, ec ) )
        continue;

      // Iterate through device symlinks
      for ( const auto &entry : fs::directory_iterator( driverPath, ec ) )
      {
        if ( !entry.is_symlink( ec ) )
          continue;

        auto ledsPath = entry.path() / "leds";
        if ( !fs::exists( ledsPath, ec ) )
          continue;

        // Find all rgb:kbd_backlight* directories
        std::vector< std::pair< std::string, int > > foundLEDs;
        for ( const auto &ledEntry : fs::directory_iterator( ledsPath, ec ) )
        {
          std::string name = ledEntry.path().filename().string();
          if ( name.find( "rgb:kbd_backlight" ) == std::string::npos )
            continue;

          // Extract zone number for sorting
          int zoneNum = 0;
          if ( name != "rgb:kbd_backlight" )
          {
            size_t underscorePos = name.find_last_of( '_' );
            if ( underscorePos != std::string::npos )
            {
              try
              {
                zoneNum = std::stoi( name.substr( underscorePos + 1 ) );
              }
              catch ( ... )
              {
                zoneNum = 0;
              }
            }
          }

          foundLEDs.push_back( { ledEntry.path().string(), zoneNum } );
        }

        // Sort by zone number
        std::sort( foundLEDs.begin(), foundLEDs.end(),
                  []( const auto &a, const auto &b ) { return a.second < b.second; } );

        // Add sorted paths
        for ( const auto &led : foundLEDs )
        {
          m_ledPaths.push_back( led.first );
        }

        if ( !m_ledPaths.empty() )
          return;  // Found per-key LEDs
      }
    }
  }

  std::string capabilitiesToJSON() const
  {
    if ( m_capabilities.zones == 0 )
      return "null";

    std::ostringstream oss;
    oss << "{\"modes\":[0]";  // Static mode only for now
    oss << ",\"zones\":" << m_capabilities.zones;
    oss << ",\"maxBrightness\":" << m_capabilities.maxBrightness;

    if ( m_capabilities.maxRed > 0 )
    {
      oss << ",\"maxRed\":" << m_capabilities.maxRed;
      oss << ",\"maxGreen\":" << m_capabilities.maxGreen;
      oss << ",\"maxBlue\":" << m_capabilities.maxBlue;
    }

    oss << "}";
    return oss.str();
  }

  std::string statesToJSON( const std::vector< KeyboardBacklightState > &states ) const
  {
    std::ostringstream oss;
    oss << "[";
    for ( size_t i = 0; i < states.size(); ++i )
    {
      if ( i > 0 )
        oss << ",";
      oss << "{"
          << "\"brightness\":" << states[i].brightness << ","
          << "\"red\":" << states[i].red << ","
          << "\"green\":" << states[i].green << ","
          << "\"blue\":" << states[i].blue
          << "}";
    }
    oss << "]";
    return oss.str();
  }

  void applyStatesFromJSON( const std::string &statesJSON )
  {
    if ( !m_getControlEnabled() )
    {
      std::cout << "[KeyboardBacklight] Control disabled, ignoring state update" << std::endl;
      return;
    }

    if ( statesJSON.empty() || statesJSON == "[]" )
      return;

    try
    {
      // Simple manual parsing for array of state objects
      std::vector< KeyboardBacklightState > newStates;
      
      size_t pos = 0;
      while ( ( pos = statesJSON.find( '{', pos ) ) != std::string::npos )
      {
        size_t end = statesJSON.find( '}', pos );
        if ( end == std::string::npos )
          break;

        std::string stateObj = statesJSON.substr( pos, end - pos + 1 );
        KeyboardBacklightState kbs;
        
        kbs.brightness = extractInt( stateObj, "brightness" );
        kbs.red = extractInt( stateObj, "red" );
        kbs.green = extractInt( stateObj, "green" );
        kbs.blue = extractInt( stateObj, "blue" );
        
        newStates.push_back( kbs );
        pos = end + 1;
      }

      if ( !newStates.empty() )
      {
        applyStates( newStates );
        m_currentStates = newStates;

        // Update DBus data
        m_setStatesJSON( statesJSON );
      }
    }
    catch ( const std::exception &e )
    {
      std::cerr << "[KeyboardBacklight] Error parsing states JSON: " << e.what() << std::endl;
    }
  }

  std::string extractStatesFromKeyboardJSON( const std::string &keyboardJSON ) const
  {
    // Look for "states" array in the keyboard JSON
    std::string search = "\"states\":";
    size_t pos = keyboardJSON.find( search );
    if ( pos == std::string::npos )
    {
      // Try alternative format where states might be at root level
      if ( keyboardJSON.find( '[' ) == 0 )
      {
        return keyboardJSON;  // The whole JSON is the states array
      }
      return "";
    }

    pos += search.length();
    
    // Find the start of the array
    size_t arrayStart = keyboardJSON.find( '[', pos );
    if ( arrayStart == std::string::npos )
      return "";

    // Find the matching closing bracket
    size_t arrayEnd = arrayStart;
    int depth = 0;
    for ( size_t i = arrayStart; i < keyboardJSON.length(); ++i )
    {
      if ( keyboardJSON[i] == '[' ) ++depth;
      else if ( keyboardJSON[i] == ']' ) --depth;
      if ( depth == 0 )
      {
        arrayEnd = i;
        break;
      }
    }

    if ( arrayEnd > arrayStart )
    {
      return keyboardJSON.substr( arrayStart, arrayEnd - arrayStart + 1 );
    }

    return "";
  }

  int extractInt( const std::string &json, const std::string &key ) const
  {
    std::string search = "\"" + key + "\":";
    size_t pos = json.find( search );
    if ( pos == std::string::npos )
      return 0;

    pos += search.length();
    size_t endPos = json.find_first_of( ",}", pos );
    if ( endPos == std::string::npos )
      return 0;

    std::string valueStr = json.substr( pos, endPos - pos );
    try
    {
      return std::stoi( valueStr );
    }
    catch ( ... )
    {
      return 0;
    }
  }

  void applyStates( const std::vector< KeyboardBacklightState > &states )
  {
    if ( states.empty() || m_ledPaths.empty() )
      return;

    // Set brightness (all zones use the same brightness from first state)
    setBrightness( states[0].brightness );

    // Set colors for RGB keyboards
    if ( m_capabilities.maxRed > 0 )
    {
      setBufferInput( true );

      for ( size_t i = 0; i < m_ledPaths.size() && i < states.size(); ++i )
      {
        setMultiIntensity( i, states[i].red, states[i].green, states[i].blue );
      }

      setBufferInput( false );
    }
  }

  void setBrightness( int brightness )
  {
    if ( m_ledPaths.empty() )
      return;

    // All zones share the same brightness control (use first path)
    SysfsNode< int > brightnessNode( m_ledPaths[0] + "/brightness" );
    if ( !brightnessNode.write( brightness ) )
    {
      std::cerr << "[KeyboardBacklight] Failed to set brightness to " << brightness << std::endl;
    }
  }

  void setMultiIntensity( size_t zoneIndex, int red, int green, int blue )
  {
    if ( zoneIndex >= m_ledPaths.size() )
      return;

    std::string multiIntensityPath = m_ledPaths[zoneIndex] + "/multi_intensity";
    std::error_code ec;

    if ( !fs::exists( multiIntensityPath, ec ) )
      return;

    std::string value = std::to_string( red ) + " " + 
                       std::to_string( green ) + " " + 
                       std::to_string( blue );

    std::ofstream file( multiIntensityPath, std::ios::app );
    if ( file.is_open() )
    {
      file << value;
      file.close();
    }
  }

  void setBufferInput( bool bufferOn )
  {
    if ( m_ledPaths.empty() )
      return;

    // Buffer control is usually in the device directory
    std::string bufferPath = m_ledPaths[0] + "/device/controls/buffer_input";
    std::error_code ec;

    if ( !fs::exists( bufferPath, ec ) )
      return;

    std::ofstream file( bufferPath, std::ios::app );
    if ( file.is_open() )
    {
      file << ( bufferOn ? "1" : "0" );
      file.close();
    }
  }
};
