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
#include <vector>
#include <optional>
#include <filesystem>
#include <fstream>

/**
 * @brief Power supply type enumeration
 */
enum class PowerSupplyType
{
  Unknown,
  Mains,
  Battery,
};

/**
 * @brief Charge type enumeration
 *
 * Definitions as of 2023-08-11 from
 *   https://www.kernel.org/doc/Documentation/ABI/testing/sysfs-class-power
 *   Section: /sys/class/power_supply/<supply_name>/charge_type
 */
enum class ChargeType
{
  Unknown,
  NotAvailable,
  Trickle,
  Fast,
  Standard,
  Adaptive,
  Custom,
  LongLife,
  Bypass,
};

/**
 * @brief Power Supply Controller
 *
 * Provides access to power supply information via sysfs.
 * Handles battery charge control and power supply status monitoring.
 */
class PowerSupplyController
{
public:
  /**
   * @brief Constructor
   * @param basePath Base path to power supply sysfs directory
   */
  explicit PowerSupplyController( const std::string &basePath ) noexcept
    : m_basePath( basePath )
  {
  }

  virtual ~PowerSupplyController() = default;

  // Allow move but not copy (for returning from functions)
  PowerSupplyController( const PowerSupplyController & ) = delete;
  PowerSupplyController( PowerSupplyController && ) = default;
  PowerSupplyController &operator=( const PowerSupplyController & ) = delete;
  PowerSupplyController &operator=( PowerSupplyController && ) = default;

  /**
   * @brief Get the base path
   * @return Base sysfs path for this power supply
   */
  [[nodiscard]] const std::string &getBasePath() const noexcept
  {
    return m_basePath;
  }

  /**
   * @brief Check if power supply is online
   * @return true if online, false otherwise
   */
  [[nodiscard]] bool isOnline() const noexcept
  {
    return SysfsNode< int64_t >( m_basePath + "/online" ).read().value_or( 0 ) != 0;
  }

  /**
   * @brief Get power supply type
   * @return PowerSupplyType enum value
   */
  [[nodiscard]] PowerSupplyType getType() const noexcept
  {
    const std::string typeStr = SysfsNode< std::string >( m_basePath + "/type" ).read().value_or( "" );
    
    if ( typeStr == "Battery" )
      return PowerSupplyType::Battery;
    else if ( typeStr == "Mains" )
      return PowerSupplyType::Mains;
    
    return PowerSupplyType::Unknown;
  }

  /**
   * @brief Get charge control start threshold
   * @return Start threshold percentage or -1 if not available
   */
  [[nodiscard]] int getChargeControlStartThreshold() const noexcept
  {
    return static_cast< int >( SysfsNode< int64_t >( m_basePath + "/charge_control_start_threshold" ).read().value_or( -1 ) );
  }

  /**
   * @brief Set charge control start threshold
   * @param threshold Start threshold percentage (0-100)
   * @return true if successful, false otherwise
   */
  [[nodiscard]] bool setChargeControlStartThreshold( int threshold ) const noexcept
  {
    return SysfsNode< int64_t >( m_basePath + "/charge_control_start_threshold" ).write( threshold );
  }

  /**
   * @brief Get charge control end threshold
   * @return End threshold percentage or -1 if not available
   */
  [[nodiscard]] int getChargeControlEndThreshold() const noexcept
  {
    return static_cast< int >( SysfsNode< int64_t >( m_basePath + "/charge_control_end_threshold" ).read().value_or( -1 ) );
  }

  /**
   * @brief Set charge control end threshold
   * @param threshold End threshold percentage (0-100)
   * @return true if successful, false otherwise
   */
  [[nodiscard]] bool setChargeControlEndThreshold( int threshold ) const noexcept
  {
    return SysfsNode< int64_t >( m_basePath + "/charge_control_end_threshold" ).write( threshold );
  }

  /**
   * @brief Get charge type
   * @return ChargeType enum value
   */
  [[nodiscard]] ChargeType getChargeType() const noexcept
  {
    const std::string chargeTypeStr = SysfsNode< std::string >( m_basePath + "/charge_type" ).read().value_or( "" );
    
    if ( chargeTypeStr == "Trickle" )
      return ChargeType::Trickle;
    else if ( chargeTypeStr == "Fast" )
      return ChargeType::Fast;
    else if ( chargeTypeStr == "Standard" )
      return ChargeType::Standard;
    else if ( chargeTypeStr == "Adaptive" )
      return ChargeType::Adaptive;
    else if ( chargeTypeStr == "Custom" )
      return ChargeType::Custom;
    else if ( chargeTypeStr == "LongLife" )
      return ChargeType::LongLife;
    else if ( chargeTypeStr == "Bypass" )
      return ChargeType::Bypass;
    else if ( chargeTypeStr == "N/A" )
      return ChargeType::NotAvailable;
    
    return ChargeType::Unknown;
  }

  /**
   * @brief Get available start thresholds (unofficial interface)
   * @return Vector of available threshold values
   */
  [[nodiscard]] std::vector< int > getChargeControlStartAvailableThresholds() const noexcept
  {
    auto values = SysfsNode< std::vector< int32_t > >( m_basePath + "/charge_control_start_available_thresholds", " " ).read();
    if ( not values )
      return {};
    
    // Convert int32_t to int
    std::vector< int > result;
    result.reserve( values->size() );
    for ( int32_t v : *values )
      result.push_back( v );
    return result;
  }

  /**
   * @brief Get available end thresholds (unofficial interface)
   * @return Vector of available threshold values
   */
  [[nodiscard]] std::vector< int > getChargeControlEndAvailableThresholds() const noexcept
  {
    auto values = SysfsNode< std::vector< int32_t > >( m_basePath + "/charge_control_end_available_thresholds", " " ).read();
    if ( not values )
      return {};
    
    // Convert int32_t to int
    std::vector< int > result;
    result.reserve( values->size() );
    for ( int32_t v : *values )
      result.push_back( v );
    return result;
  }

  /**
   * @brief Get all battery power supplies
   * @return Vector of PowerSupplyController instances for batteries
   */
  [[nodiscard]] static std::vector< PowerSupplyController > getPowerSupplyBatteries() noexcept
  {
    std::vector< PowerSupplyController > batteries;
    const std::string psPath = "/sys/class/power_supply";

    try
    {
      if ( not std::filesystem::exists( psPath ) )
        return batteries;

      for ( const auto &entry : std::filesystem::directory_iterator( psPath ) )
      {
        if ( entry.is_directory() or entry.is_symlink() )
        {
          if ( PowerSupplyController ps( entry.path().string() ); ps.getType() == PowerSupplyType::Battery )
            batteries.push_back( std::move( ps ) );
        }
      }
    }
    catch ( const std::exception & )
    {
      // Return empty vector on error
    }

    return batteries;
  }

  /**
   * @brief Get the first battery power supply
   * @return Optional PowerSupplyController for first battery, or nullopt if none found
   */
  [[nodiscard]] static std::optional< PowerSupplyController > getFirstBattery() noexcept
  {
    auto batteries = getPowerSupplyBatteries();
    
    if ( not batteries.empty() )
      return std::move( batteries[ 0 ] );
    
    return std::nullopt;
  }

private:
  std::string m_basePath;
};
