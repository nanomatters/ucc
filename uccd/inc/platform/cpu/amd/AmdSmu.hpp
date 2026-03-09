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

#include <array>
#include <cerrno>
#include <cpuid.h>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <optional>
#include <string>
#include <syslog.h>
#include <thread>
#include <unistd.h>

namespace ucc::hal
{

/**
 * @brief Direct AMD SMU (System Management Unit) communication.
 *
 * Replaces the dlopen-based RyzenAdj wrapper with a self-contained
 * implementation that talks to the SMU via PCI config-space indirect
 * SMN (System Management Network) register access.
 *
 * Protocol:
 *   1. Write 32-bit SMN address to PCI config offset 0xB8 of device 00:00.0
 *   2. Read/write 32-bit data from/to PCI config offset 0xBC
 *
 * The SMU mailbox handshake:
 *   1. Clear the response register         (write 0 to REP)
 *   2. Write argument(s) to ARG0..ARG5     (arg_base + 0*4 .. 5*4)
 *   3. Write the message ID to MSG register
 *   4. Poll REP until non-zero
 *   5. Read back ARG0..ARG5 for results
 *
 * No external library dependencies.  Requires root access to PCI config space.
 *
 * Reference: RyzenAdj project (LGPL)
 *   - nb_smu_ops.c — SMN register access and mailbox protocol
 *   - cpuid.c      — AMD CPU family/model identification
 *   - api.c        — SMU message IDs per family
 */
class AmdSmu
{
public:
  // ---- AMD CPU families we support ----
  enum class Family
  {
    Unknown,
    Raven,         // Fam 17h / Zen
    Picasso,       // Fam 17h / Zen+
    Dali,          // Fam 17h / Zen
    Renoir,        // Fam 17h / Zen2
    Lucienne,      // Fam 17h / Zen2
    VanGogh,       // Fam 17h / Zen2
    Mendocino,     // Fam 17h / Zen2
    Cezanne,       // Fam 19h / Zen3
    Rembrandt,     // Fam 19h / Zen3+
    DragonRange,   // Fam 19h / Zen4 desktop
    Phoenix,       // Fam 19h / Zen4 mobile
    Hawkpoint,     // Fam 19h / Zen4 mobile
    StrixPoint,    // Fam 1Ah / Zen5 mobile
    FireRange,     // Fam 1Ah / Zen5 desktop
    KrackanPoint,  // Fam 1Ah / Zen5 mobile
    StrixHalo,     // Fam 1Ah / Zen5 mobile
  };

  // ---- SMU response codes ----
  static constexpr uint32_t REP_OK          = 0x01;
  static constexpr uint32_t REP_FAILED      = 0xFF;
  static constexpr uint32_t REP_UNKNOWN_CMD = 0xFE;
  static constexpr uint32_t REP_REJECTED    = 0xFD;
  static constexpr uint32_t REP_BUSY        = 0xFC;

  AmdSmu() = default;

  ~AmdSmu() { close(); }

  // Non-copyable
  AmdSmu( const AmdSmu & ) = delete;
  AmdSmu &operator=( const AmdSmu & ) = delete;

  /**
   * @brief Detect AMD CPU and initialise SMU mailbox access.
   *
   * 1. CPUID to identify the exact Ryzen family.
   * 2. Open PCI config space for 00:00.0 (the AMD Data Fabric / NB).
   * 3. Look up the correct MP1 and PSMU mailbox addresses for the family.
   * 4. Send a test message to verify the mailbox is responsive.
   */
  bool init()
  {
    if ( m_fd >= 0 )
      return true; // already initialised

    m_family = identifyFamily();
    if ( m_family == Family::Unknown )
    {
      syslog( LOG_DEBUG, "[AmdSmu] not a supported AMD CPU" );
      return false;
    }

    // Open PCI config space for 00:00.0
    m_fd = ::open( PCI_CONFIG_PATH, O_RDWR );
    if ( m_fd < 0 )
    {
      syslog( LOG_WARNING, "[AmdSmu] cannot open %s: %s", PCI_CONFIG_PATH, strerror( errno ) );
      return false;
    }

    // Look up mailbox addresses for this family
    if ( !setupMailboxAddresses() )
    {
      syslog( LOG_WARNING, "[AmdSmu] unsupported family for mailbox setup" );
      close();
      return false;
    }

    // Verify MP1 mailbox is responsive
    if ( !smuTest( m_mp1 ) )
    {
      syslog( LOG_WARNING, "[AmdSmu] MP1 mailbox test failed" );
      close();
      return false;
    }

    // Verify PSMU mailbox is responsive
    if ( !smuTest( m_psmu ) )
    {
      syslog( LOG_WARNING, "[AmdSmu] PSMU mailbox test failed — will continue with MP1 only" );
      m_psmuAvailable = false;
    }
    else
    {
      m_psmuAvailable = true;
    }

    syslog( LOG_INFO, "[AmdSmu] initialised — family %s, MP1 OK, PSMU %s",
            familyName(), m_psmuAvailable ? "OK" : "unavailable" );
    return true;
  }

  void close()
  {
    if ( m_fd >= 0 )
    {
      ::close( m_fd );
      m_fd = -1;
    }
  }

  bool isInitialised() const { return m_fd >= 0; }

  Family family() const { return m_family; }

  const char *familyName() const
  {
    switch ( m_family )
    {
      case Family::Raven:        return "Raven";
      case Family::Picasso:      return "Picasso";
      case Family::Dali:         return "Dali";
      case Family::Renoir:       return "Renoir";
      case Family::Lucienne:     return "Lucienne";
      case Family::VanGogh:      return "VanGogh";
      case Family::Mendocino:    return "Mendocino";
      case Family::Cezanne:      return "Cezanne";
      case Family::Rembrandt:    return "Rembrandt";
      case Family::DragonRange:  return "DragonRange";
      case Family::Phoenix:      return "Phoenix";
      case Family::Hawkpoint:    return "Hawkpoint";
      case Family::StrixPoint:   return "StrixPoint";
      case Family::FireRange:    return "FireRange";
      case Family::KrackanPoint: return "KrackanPoint";
      case Family::StrixHalo:    return "StrixHalo";
      default:                   return "Unknown";
    }
  }

  // ---------------------------------------------------------------
  //  TDP SET operations  (value in milliwatts)
  // ---------------------------------------------------------------

  bool setStapmLimit( uint32_t mw )
  {
    auto msgId = stapmMsgId();
    if ( !msgId ) return false;
    return smuSend( m_mp1, *msgId, mw ) == REP_OK;
  }

  bool setFastLimit( uint32_t mw )
  {
    auto msgId = fastMsgId();
    if ( !msgId ) return false;
    return smuSend( m_mp1, *msgId, mw ) == REP_OK;
  }

  bool setSlowLimit( uint32_t mw )
  {
    auto msgId = slowMsgId();
    if ( !msgId ) return false;
    return smuSend( m_mp1, *msgId, mw ) == REP_OK;
  }

  bool setTctlTemp( uint32_t degreesC )
  {
    auto msgId = tctlMsgId();
    if ( !msgId ) return false;
    return smuSend( m_mp1, *msgId, degreesC ) == REP_OK;
  }

private:
  // ---- PCI config space for SMN indirect register access ----
  static constexpr const char *PCI_CONFIG_PATH = "/sys/bus/pci/devices/0000:00:00.0/config";
  static constexpr uint32_t NB_ADDR_REG = 0xB8; // Write SMN address here
  static constexpr uint32_t NB_DATA_REG = 0xBC; // Read/write SMN data here

  // ---- SMU mailbox register set ----
  struct Mailbox
  {
    uint32_t msg;      // Message ID register (SMN address)
    uint32_t rep;      // Response register
    uint32_t argBase;  // Base of 6 argument registers (arg0..arg5 at +0, +4, +8, +12, +16, +20)
  };

  // ---- MP1 mailbox address sets ----
  // Set 1: Raven / Picasso / Dali / Renoir / Lucienne / Cezanne (default)
  static constexpr Mailbox MP1_SET_1 = { 0x3B10528, 0x3B10564, 0x3B10998 };
  // Set 2: Rembrandt / VanGogh / Mendocino / Phoenix / Hawkpoint
  static constexpr Mailbox MP1_SET_2 = { 0x3B10528, 0x3B10578, 0x3B10998 };
  // Set 3: KrackanPoint / StrixPoint / StrixHalo
  static constexpr Mailbox MP1_SET_3 = { 0x3B10928, 0x3B10978, 0x3B10998 };
  // Set 4: DragonRange / FireRange (desktop Zen4/5)
  static constexpr Mailbox MP1_SET_4 = { 0x3B10530, 0x3B1057C, 0x3B109C4 };

  // ---- PSMU mailbox address sets ----
  // Set 1: Default (most mobile families)
  static constexpr Mailbox PSMU_SET_1 = { 0x3B10A20, 0x3B10A80, 0x3B10A88 };
  // Set 2: DragonRange / FireRange
  static constexpr Mailbox PSMU_SET_2 = { 0x3B10524, 0x3B10570, 0x3B10A40 };

  // ---- SMN register I/O via PCI config space ----

  uint32_t smnRead( uint32_t addr ) const
  {
    uint32_t aligned = addr & ~uint32_t( 0x3 );
    pciWriteLong( NB_ADDR_REG, aligned );
    return pciReadLong( NB_DATA_REG );
  }

  void smnWrite( uint32_t addr, uint32_t data ) const
  {
    pciWriteLong( NB_ADDR_REG, addr );
    pciWriteLong( NB_DATA_REG, data );
  }

  void pciWriteLong( uint32_t offset, uint32_t value ) const
  {
    if ( ::lseek( m_fd, static_cast< off_t >( offset ), SEEK_SET ) < 0 )
    {
      syslog( LOG_WARNING, "[AmdSmu] pciWriteLong lseek failed: off=0x%X err=%s", offset, std::strerror( errno ) );
      return;
    }

    size_t written = 0;
    const auto *buffer = reinterpret_cast< const uint8_t * >( &value );
    while ( written < sizeof( value ) )
    {
      const ssize_t rc = ::write( m_fd, buffer + written, sizeof( value ) - written );
      if ( rc < 0 )
      {
        if ( errno == EINTR )
          continue;
        syslog( LOG_WARNING, "[AmdSmu] pciWriteLong write failed: off=0x%X err=%s", offset, std::strerror( errno ) );
        return;
      }
      if ( rc == 0 )
      {
        syslog( LOG_WARNING, "[AmdSmu] pciWriteLong short write: off=0x%X", offset );
        return;
      }
      written += static_cast< size_t >( rc );
    }
  }

  uint32_t pciReadLong( uint32_t offset ) const
  {
    uint32_t value = 0;

    if ( ::lseek( m_fd, static_cast< off_t >( offset ), SEEK_SET ) < 0 )
    {
      syslog( LOG_WARNING, "[AmdSmu] pciReadLong lseek failed: off=0x%X err=%s", offset, std::strerror( errno ) );
      return value;
    }

    size_t readBytes = 0;
    auto *buffer = reinterpret_cast< uint8_t * >( &value );
    while ( readBytes < sizeof( value ) )
    {
      const ssize_t rc = ::read( m_fd, buffer + readBytes, sizeof( value ) - readBytes );
      if ( rc < 0 )
      {
        if ( errno == EINTR )
          continue;
        syslog( LOG_WARNING, "[AmdSmu] pciReadLong read failed: off=0x%X err=%s", offset, std::strerror( errno ) );
        return value;
      }
      if ( rc == 0 )
      {
        syslog( LOG_WARNING, "[AmdSmu] pciReadLong short read: off=0x%X", offset );
        return value;
      }
      readBytes += static_cast< size_t >( rc );
    }

    return value;
  }

  // ---- SMU mailbox protocol ----

  /**
   * Send a command to an SMU mailbox.
   * @param mb   The mailbox (MP1 or PSMU).
   * @param msgId The SMU message ID.
   * @param arg0  The first argument (typically the value).
   * @return The response code (REP_OK on success).
   */
  uint32_t smuSend( const Mailbox &mb, uint32_t msgId, uint32_t arg0 ) const
  {
    // Step 1: Clear the response register
    smnWrite( mb.rep, 0x0 );

    // Step 2: Write arguments (arg0 = value, arg1..5 = 0)
    smnWrite( mb.argBase + 0 * 4, arg0 );
    smnWrite( mb.argBase + 1 * 4, 0 );
    smnWrite( mb.argBase + 2 * 4, 0 );
    smnWrite( mb.argBase + 3 * 4, 0 );
    smnWrite( mb.argBase + 4 * 4, 0 );
    smnWrite( mb.argBase + 5 * 4, 0 );

    // Step 3: Send the message ID
    smnWrite( mb.msg, msgId );

    // Step 4: Poll for response (with timeout)
    uint32_t response = 0;
    for ( int i = 0; i < MAX_POLL_ITERATIONS && response == 0; ++i )
    {
      response = smnRead( mb.rep );
      if ( response == 0 )
        std::this_thread::sleep_for( std::chrono::microseconds( 100 ) );
    }

    // Read back argument registers for diagnostics
    uint32_t args[6];
    for ( int i = 0; i < 6; ++i )
      args[i] = smnRead( mb.argBase + static_cast<uint32_t>( i ) * 4 );

    if ( response == 0 )
    {
      syslog( LOG_WARNING, "[AmdSmu] smuSend timeout: msg=0x%X arg0=0x%X resp=0x%X args=0x%X,0x%X,0x%X,0x%X,0x%X,0x%X",
              msgId, arg0, response,
              args[0], args[1], args[2], args[3], args[4], args[5] );
    }
    else
    {
      syslog( LOG_DEBUG, "[AmdSmu] smuSend: msg=0x%X arg0=0x%X resp=0x%X args=0x%X,0x%X,0x%X,0x%X,0x%X,0x%X",
              msgId, arg0, response,
              args[0], args[1], args[2], args[3], args[4], args[5] );
    }

    return response;
  }

  static constexpr int MAX_POLL_ITERATIONS = 10000; // ~1 second at 100µs per iteration

  /**
   * Send the test message (0x1) to verify the mailbox is responsive.
   */
  bool smuTest( const Mailbox &mb ) const
  {
    // Clear response
    smnWrite( mb.rep, 0x0 );

    // Write a known value to arg0 and read it back to check PCI bus is writable
    smnWrite( mb.argBase, 0x47 );
    if ( smnRead( mb.argBase ) != 0x47 )
    {
      syslog( LOG_WARNING, "[AmdSmu] PCI bus not writable (Secure Boot?)" );
      return false;
    }

    // Send test message (0x1)
    smnWrite( mb.msg, 0x1 );

    // Poll for response
    uint32_t response = 0;
    for ( int i = 0; i < MAX_POLL_ITERATIONS && response == 0; ++i )
    {
      response = smnRead( mb.rep );
      if ( response == 0 )
        std::this_thread::sleep_for( std::chrono::microseconds( 100 ) );
    }

    // Read back args for troubleshooting
    uint32_t args[6];
    for ( int i = 0; i < 6; ++i )
      args[i] = smnRead( mb.argBase + static_cast<uint32_t>( i ) * 4 );

    if ( response == 0 )
      syslog( LOG_WARNING, "[AmdSmu] smuTest timeout: resp=0x%X args=0x%X,0x%X,0x%X,0x%X,0x%X,0x%X",
              response, args[0], args[1], args[2], args[3], args[4], args[5] );
    else
      syslog( LOG_DEBUG, "[AmdSmu] smuTest response=0x%X args=0x%X,0x%X,0x%X,0x%X,0x%X,0x%X",
              response, args[0], args[1], args[2], args[3], args[4], args[5] );

    return response == REP_OK;
  }

  // ---- Family-specific SMU message IDs ----
  // These are the MP1 message IDs for set_stapm_limit, set_fast_limit,
  // set_slow_limit, set_tctl_temp per CPU family.

  std::optional< uint32_t > stapmMsgId() const
  {
    switch ( m_family )
    {
      case Family::Raven:
      case Family::Picasso:
      case Family::Dali:         return 0x1A;
      case Family::Renoir:
      case Family::Lucienne:
      case Family::Cezanne:      return 0x14;
      case Family::Rembrandt:
      case Family::VanGogh:
      case Family::Mendocino:
      case Family::Phoenix:
      case Family::Hawkpoint:
      case Family::KrackanPoint:
      case Family::StrixPoint:
      case Family::StrixHalo:    return 0x14;
      case Family::DragonRange:
      case Family::FireRange:    return 0x4F;
      default:                   return std::nullopt;
    }
  }

  std::optional< uint32_t > fastMsgId() const
  {
    switch ( m_family )
    {
      case Family::Raven:
      case Family::Picasso:
      case Family::Dali:         return 0x1B;
      case Family::Renoir:
      case Family::Lucienne:
      case Family::Cezanne:      return 0x15;
      case Family::Rembrandt:
      case Family::VanGogh:
      case Family::Mendocino:
      case Family::Phoenix:
      case Family::Hawkpoint:
      case Family::KrackanPoint:
      case Family::StrixPoint:
      case Family::StrixHalo:    return 0x15;
      case Family::DragonRange:
      case Family::FireRange:    return 0x3E;
      default:                   return std::nullopt;
    }
  }

  std::optional< uint32_t > slowMsgId() const
  {
    switch ( m_family )
    {
      case Family::Raven:
      case Family::Picasso:
      case Family::Dali:         return 0x1C;
      case Family::Renoir:
      case Family::Lucienne:
      case Family::Cezanne:      return 0x16;
      case Family::Rembrandt:
      case Family::VanGogh:
      case Family::Mendocino:
      case Family::Phoenix:
      case Family::Hawkpoint:
      case Family::KrackanPoint:
      case Family::StrixPoint:
      case Family::StrixHalo:    return 0x16;
      case Family::DragonRange:
      case Family::FireRange:    return 0x5F;
      default:                   return std::nullopt;
    }
  }

  std::optional< uint32_t > tctlMsgId() const
  {
    switch ( m_family )
    {
      case Family::Raven:
      case Family::Picasso:
      case Family::Dali:         return 0x1E;
      case Family::Renoir:
      case Family::Lucienne:
      case Family::Cezanne:      return 0x18;
      case Family::Rembrandt:
      case Family::VanGogh:
      case Family::Mendocino:
      case Family::Phoenix:
      case Family::Hawkpoint:
      case Family::KrackanPoint:
      case Family::StrixPoint:
      case Family::StrixHalo:    return 0x18;
      case Family::DragonRange:
      case Family::FireRange:    return 0x3F;
      default:                   return std::nullopt;
    }
  }

  // ---- Mailbox address selection per family ----

  bool setupMailboxAddresses()
  {
    switch ( m_family )
    {
      case Family::Raven:
      case Family::Picasso:
      case Family::Dali:
      case Family::Renoir:
      case Family::Lucienne:
      case Family::Cezanne:
        m_mp1  = MP1_SET_1;
        m_psmu = PSMU_SET_1;
        return true;

      case Family::Rembrandt:
      case Family::VanGogh:
      case Family::Mendocino:
      case Family::Phoenix:
      case Family::Hawkpoint:
        m_mp1  = MP1_SET_2;
        m_psmu = PSMU_SET_1;
        return true;

      case Family::KrackanPoint:
      case Family::StrixPoint:
      case Family::StrixHalo:
        m_mp1  = MP1_SET_3;
        m_psmu = PSMU_SET_1;
        return true;

      case Family::DragonRange:
      case Family::FireRange:
        m_mp1  = MP1_SET_4;
        m_psmu = PSMU_SET_2;
        return true;

      default:
        return false;
    }
  }

  // ---- CPU identification via CPUID ----

  static Family identifyFamily()
  {
    // Check vendor is AMD
    if ( !isAmdCpu() )
      return Family::Unknown;

    // CPUID leaf 1: get family and model
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    __cpuid( 1, eax, ebx, ecx, edx );

    int family = static_cast< int >( ( ( eax >> 8 ) & 0xF ) + ( ( eax >> 20 ) & 0xFF ) );
    int model  = static_cast< int >( ( ( eax >> 4 ) & 0xF ) | ( ( eax >> 12 ) & 0xF0 ) );

    switch ( family )
    {
      case 0x17: // Zen, Zen+, Zen2
        switch ( model )
        {
          case 17:  return Family::Raven;
          case 24:  return Family::Picasso;
          case 32:  return Family::Dali;
          case 96:  return Family::Renoir;
          case 104: return Family::Lucienne;
          case 144:
          case 145: return Family::VanGogh;
          case 160: return Family::Mendocino;
        }
        break;

      case 0x19: // Zen3, Zen4
        switch ( model )
        {
          case 80:  return Family::Cezanne;
          case 64:
          case 68:  return Family::Rembrandt;
          case 97:  return Family::DragonRange;
          case 116:
          case 120: return Family::Phoenix;
          case 117: return Family::Hawkpoint;
        }
        break;

      case 0x1A: // Zen5, Zen6
        switch ( model )
        {
          case 32:
          case 36:  return Family::StrixPoint;
          case 68:  return Family::FireRange;
          case 96:  return Family::KrackanPoint;
          case 112: return Family::StrixHalo;
        }
        break;
    }

    syslog( LOG_DEBUG, "[AmdSmu] AMD CPU family 0x%X model %d not in our table", family, model );
    return Family::Unknown;
  }

  static bool isAmdCpu()
  {
    uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
    __cpuid( 0, eax, ebx, ecx, edx );

    // "AuthenticAMD" = EBX:"Auth" + EDX:"enti" + ECX:"cAMD"
    char vendor[13];
    std::memcpy( vendor + 0, &ebx, 4 );
    std::memcpy( vendor + 4, &edx, 4 );
    std::memcpy( vendor + 8, &ecx, 4 );
    vendor[12] = '\0';

    return std::strncmp( vendor, "AuthenticAMD", 12 ) == 0;
  }

  int m_fd = -1;
  Family m_family = Family::Unknown;
  bool m_psmuAvailable = false;

  Mailbox m_mp1  = {};
  Mailbox m_psmu = {};
};

} // namespace ucc::hal
