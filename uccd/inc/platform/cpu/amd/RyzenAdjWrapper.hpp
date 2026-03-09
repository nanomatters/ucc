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

#include <cstdint>
#include <dlfcn.h>
#include <optional>
#include <string>
#include <syslog.h>

namespace ucc::hal
{

/**
 * @brief Thin dlopen-based wrapper around libryzenadj.
 *
 * Loads the shared library at runtime so that UCC has no hard link-time
 * dependency on RyzenAdj.  All AMD SMU TDP operations go through here.
 *
 * Lifetime: created once, held by AmdCpuPlatformProvider.
 */
class RyzenAdjWrapper
{
public:
  RyzenAdjWrapper() = default;

  ~RyzenAdjWrapper()
  {
    close();
  }

  // non-copyable
  RyzenAdjWrapper( const RyzenAdjWrapper & ) = delete;
  RyzenAdjWrapper &operator=( const RyzenAdjWrapper & ) = delete;

  /**
   * @brief Load libryzenadj and initialise SMU access.
   * @return true if the library was loaded and the SMU was initialised.
   */
  bool init()
  {
    if ( m_ry )
      return true; // already initialised

    m_handle = dlopen( "libryzenadj.so", RTLD_NOW );
    if ( !m_handle )
    {
      syslog( LOG_DEBUG, "[RyzenAdj] dlopen failed: %s", dlerror() );
      return false;
    }

    // Resolve symbols
    if ( !loadSymbols() )
    {
      syslog( LOG_WARNING, "[RyzenAdj] failed to resolve all symbols" );
      dlclose( m_handle );
      m_handle = nullptr;
      return false;
    }

    m_ry = m_fn_init();
    if ( !m_ry )
    {
      syslog( LOG_WARNING, "[RyzenAdj] init_ryzenadj() returned null" );
      dlclose( m_handle );
      m_handle = nullptr;
      return false;
    }

    // Initialise PM table for reading current values
    if ( m_fn_init_table( m_ry ) != 0 )
    {
      syslog( LOG_WARNING, "[RyzenAdj] init_table() failed — reading values will not work" );
      m_tableAvailable = false;
    }
    else
    {
      m_tableAvailable = true;
    }

    m_family = m_fn_get_family( m_ry );
    syslog( LOG_INFO, "[RyzenAdj] initialised — family %d, table ver 0x%x",
            static_cast< int >( m_family ), m_fn_get_table_ver( m_ry ) );
    return true;
  }

  void close()
  {
    if ( m_ry && m_fn_cleanup )
      m_fn_cleanup( m_ry );
    m_ry = nullptr;

    if ( m_handle )
      dlclose( m_handle );
    m_handle = nullptr;
  }

  bool isInitialised() const { return m_ry != nullptr; }

  int family() const { return m_family; }

  /// Whether the PM table is usable for reading current TDP values.
  /// On desktop Ryzen (Fire Range, Dragon Range) this is false because
  /// RyzenAdj has no PM table support for those families.
  bool canReadTable() const { return m_tableAvailable; }

  // ---------------------------------------------------------------
  //  TDP read (requires refresh_table first)
  // ---------------------------------------------------------------

  bool refreshTable()
  {
    if ( !m_ry || !m_fn_refresh )
      return false;
    if ( m_fn_refresh( m_ry ) != 0 )
    {
      m_tableAvailable = false;
      return false;
    }
    m_tableAvailable = true;
    return true;
  }

  /// STAPM limit in watts (sustained long-term power).
  std::optional< float > getStapmLimit()
  {
    if ( !m_ry || !m_fn_get_stapm_limit ) return std::nullopt;
    float v = m_fn_get_stapm_limit( m_ry );
    return ( v > 0.f ) ? std::optional< float >( v ) : std::nullopt;
  }

  /// STAPM actual power in watts.
  std::optional< float > getStapmValue()
  {
    if ( !m_ry || !m_fn_get_stapm_value ) return std::nullopt;
    float v = m_fn_get_stapm_value( m_ry );
    return ( v >= 0.f ) ? std::optional< float >( v ) : std::nullopt;
  }

  /// Fast PPT limit in watts (instantaneous peak).
  std::optional< float > getFastLimit()
  {
    if ( !m_ry || !m_fn_get_fast_limit ) return std::nullopt;
    float v = m_fn_get_fast_limit( m_ry );
    return ( v > 0.f ) ? std::optional< float >( v ) : std::nullopt;
  }

  /// Slow PPT limit in watts (medium-duration average).
  std::optional< float > getSlowLimit()
  {
    if ( !m_ry || !m_fn_get_slow_limit ) return std::nullopt;
    float v = m_fn_get_slow_limit( m_ry );
    return ( v > 0.f ) ? std::optional< float >( v ) : std::nullopt;
  }

  /// Tctl temperature limit in °C.
  std::optional< float > getTctlTemp()
  {
    if ( !m_ry || !m_fn_get_tctl_temp ) return std::nullopt;
    float v = m_fn_get_tctl_temp( m_ry );
    return ( v > 0.f ) ? std::optional< float >( v ) : std::nullopt;
  }

  // ---------------------------------------------------------------
  //  TDP write (values in milliwatts)
  // ---------------------------------------------------------------

  /// Set STAPM limit (sustained power) in milliwatts.
  bool setStapmLimit( uint32_t mw )
  {
    if ( !m_ry || !m_fn_set_stapm ) return false;
    return m_fn_set_stapm( m_ry, mw ) == 0;
  }

  /// Set fast PPT limit (peak power) in milliwatts.
  bool setFastLimit( uint32_t mw )
  {
    if ( !m_ry || !m_fn_set_fast ) return false;
    return m_fn_set_fast( m_ry, mw ) == 0;
  }

  /// Set slow PPT limit (average power) in milliwatts.
  bool setSlowLimit( uint32_t mw )
  {
    if ( !m_ry || !m_fn_set_slow ) return false;
    return m_fn_set_slow( m_ry, mw ) == 0;
  }

  /// Set Tctl temperature limit in °C.
  bool setTctlTemp( uint32_t degreesC )
  {
    if ( !m_ry || !m_fn_set_tctl ) return false;
    return m_fn_set_tctl( m_ry, degreesC ) == 0;
  }

private:
  // Opaque handle from RyzenAdj — struct _ryzen_access*
  using ryzen_access = void *;

  // Function pointer types matching ryzenadj.h
  using fn_init      = ryzen_access (*)();
  using fn_cleanup   = void (*)( ryzen_access );
  using fn_family    = int (*)( ryzen_access );
  using fn_init_tbl  = int (*)( ryzen_access );
  using fn_tbl_ver   = uint32_t (*)( ryzen_access );
  using fn_refresh   = int (*)( ryzen_access );
  using fn_get_float = float (*)( ryzen_access );
  using fn_set_u32   = int (*)( ryzen_access, uint32_t );

  template< typename T >
  T sym( const char *name )
  {
    // Cast through void* to avoid strict-aliasing warnings with dlsym
    void *ptr = dlsym( m_handle, name );
    T result{};
    static_assert( sizeof( T ) == sizeof( void * ) );
    std::memcpy( &result, &ptr, sizeof( T ) );
    return result;
  }

  bool loadSymbols()
  {
    m_fn_init             = sym< fn_init >( "init_ryzenadj" );
    m_fn_cleanup          = sym< fn_cleanup >( "cleanup_ryzenadj" );
    m_fn_get_family       = sym< fn_family >( "get_cpu_family" );
    m_fn_init_table       = sym< fn_init_tbl >( "init_table" );
    m_fn_get_table_ver    = sym< fn_tbl_ver >( "get_table_ver" );
    m_fn_refresh          = sym< fn_refresh >( "refresh_table" );

    m_fn_get_stapm_limit  = sym< fn_get_float >( "get_stapm_limit" );
    m_fn_get_stapm_value  = sym< fn_get_float >( "get_stapm_value" );
    m_fn_get_fast_limit   = sym< fn_get_float >( "get_fast_limit" );
    m_fn_get_slow_limit   = sym< fn_get_float >( "get_slow_limit" );
    m_fn_get_tctl_temp    = sym< fn_get_float >( "get_tctl_temp" );

    m_fn_set_stapm        = sym< fn_set_u32 >( "set_stapm_limit" );
    m_fn_set_fast         = sym< fn_set_u32 >( "set_fast_limit" );
    m_fn_set_slow         = sym< fn_set_u32 >( "set_slow_limit" );
    m_fn_set_tctl         = sym< fn_set_u32 >( "set_tctl_temp" );

    // init and cleanup are mandatory
    return m_fn_init && m_fn_cleanup && m_fn_get_family &&
           m_fn_init_table && m_fn_refresh;
  }

  void *m_handle = nullptr;
  ryzen_access m_ry = nullptr;
  int m_family = -1;
  bool m_tableAvailable = false;

  fn_init      m_fn_init = nullptr;
  fn_cleanup   m_fn_cleanup = nullptr;
  fn_family    m_fn_get_family = nullptr;
  fn_init_tbl  m_fn_init_table = nullptr;
  fn_tbl_ver   m_fn_get_table_ver = nullptr;
  fn_refresh   m_fn_refresh = nullptr;

  fn_get_float m_fn_get_stapm_limit = nullptr;
  fn_get_float m_fn_get_stapm_value = nullptr;
  fn_get_float m_fn_get_fast_limit = nullptr;
  fn_get_float m_fn_get_slow_limit = nullptr;
  fn_get_float m_fn_get_tctl_temp = nullptr;

  fn_set_u32   m_fn_set_stapm = nullptr;
  fn_set_u32   m_fn_set_fast = nullptr;
  fn_set_u32   m_fn_set_slow = nullptr;
  fn_set_u32   m_fn_set_tctl = nullptr;
};

} // namespace ucc::hal
