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

#include "hal/IPlatformProvider.hpp"
#include "platform/cpu/amd/AmdSmu.hpp"

#include <array>
#include <cmath>
#include <optional>
#include <syslog.h>

namespace ucc::hal
{

/**
 * @brief AMD Ryzen CPU platform provider — TDP control via direct SMU access.
 *
 * Provides three TDP limits exposed through the standard IPlatformProvider
 * TDP interface:
 *   0 — STAPM   (sustained / long-term)
 *   1 — PPT Fast (instantaneous peak)
 *   2 — PPT Slow (medium-duration average)
 *
 * Detection:
 *   1. CPUID to identify AMD Ryzen family/model.
 *   2. Opens PCI config space for 00:00.0 (AMD Data Fabric).
 *   3. Verifies SMU mailbox is responsive.
 *
 * No external library dependencies (no libryzenadj, no libpci).
 *
 * Priority 3 — lower than OEM providers (10) so that on Clevo/Uniwill
 * hardware the vendor driver takes precedence if both detect.  But higher
 * than 0 so it wins over providers with no TDP support at all.
 */
class AmdCpuPlatformProvider final : public IPlatformProvider
{
public:
  AmdCpuPlatformProvider() = default;

  std::string name() const override { return "amd-smu"; }

  int priority() const override { return 3; }

  bool detect() override
  {
    if ( !m_smu.init() )
    {
      syslog( LOG_INFO, "[AmdCpuPlatformProvider] AMD SMU not available" );
      return false;
    }

    // Use static defaults as initial TDP values.
    // The SMU protocol only supports SET operations — there is no generic
    // way to READ current power limits across all Ryzen families.
    m_defaults = defaultsForFamily( m_smu.family() );
    m_shadow   = m_defaults;

    syslog( LOG_INFO, "[AmdCpuPlatformProvider] detected %s — "
            "defaults: STAPM=%.0fW  Fast=%.0fW  Slow=%.0fW",
            m_smu.familyName(),
            static_cast< double >( m_defaults[ 0 ] ),
            static_cast< double >( m_defaults[ 1 ] ),
            static_cast< double >( m_defaults[ 2 ] ) );
    return true;
  }

  HwCapability capabilities() const override
  {
    return HwCapability::CpuTdpControl;
  }

  // ----- TDP control (3 limits: STAPM, Fast, Slow) -----

  int getNumberTDPs() override { return 3; }

  std::vector< std::string > getTDPDescriptors() override
  {
    return { "STAPM (Sustained)", "PPT Fast (Peak)", "PPT Slow (Average)" };
  }

  std::optional< int > getTDPMin( [[maybe_unused]] int index ) override
  {
    return 5;
  }

  std::optional< int > getTDPMax( int index ) override
  {
    if ( index < 0 || index > 2 )
      return std::nullopt;
    auto i = static_cast< size_t >( index );
    return static_cast< int >( std::ceil( m_defaults[ i ] * 2.0f ) );
  }

  std::optional< int > getTDP( int index ) override
  {
    if ( index < 0 || index > 2 )
      return std::nullopt;
    return static_cast< int >( std::round( m_shadow[ static_cast< size_t >( index ) ] ) );
  }

  bool setTDP( int index, int value ) override
  {
    if ( index < 0 || index > 2 )
      return false;

    uint32_t mw = static_cast< uint32_t >( value ) * 1000u;
    bool ok = false;

    switch ( index )
    {
      case 0: ok = m_smu.setStapmLimit( mw ); break;
      case 1: ok = m_smu.setFastLimit( mw );  break;
      case 2: ok = m_smu.setSlowLimit( mw );  break;
    }

    if ( ok )
      m_shadow[ static_cast< size_t >( index ) ] = static_cast< float >( value );

    return ok;
  }

  /// Access to the underlying SMU for extended operations.
  AmdSmu &smu() { return m_smu; }

private:
  /// Return reasonable static PPT defaults for the given AMD family.
  static std::array< float, 3 > defaultsForFamily( AmdSmu::Family fam )
  {
    // Desktop families — higher default TDP
    switch ( fam )
    {
      case AmdSmu::Family::DragonRange:
      case AmdSmu::Family::FireRange:
        // AM5 desktop: STAPM / Fast PPT / Slow PPT
        return { 142.f, 230.f, 200.f };

      default:
        // Mobile / APU families — conservative laptop defaults
        return { 25.f, 35.f, 30.f };
    }
  }

  AmdSmu m_smu;

  // Factory-like defaults for the detected family.  Watts.
  std::array< float, 3 > m_defaults = { 0.f, 0.f, 0.f };

  // Shadow values: updated on setTDP(), returned by getTDP().
  std::array< float, 3 > m_shadow = { 0.f, 0.f, 0.f };
};

} // namespace ucc::hal
