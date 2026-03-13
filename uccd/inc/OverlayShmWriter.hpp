// SPDX-License-Identifier: GPL-3.0-or-later
//
// OverlayShmWriter.hpp — Writes UccOverlayData to POSIX shared memory
// so that the ucc-fps-layer Vulkan overlay can display scan progress.

#pragma once

#include <UccOverlayData.hpp>

#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

class OverlayShmWriter
{
public:
  static OverlayShmWriter &instance()
  {
    static OverlayShmWriter s;
    return s;
  }

  /// Copy a fully populated UccOverlayData to shared memory.
  void update( const UccOverlayData &data )
  {
    if ( !ensureOpen() )
      return;
    // Write payload first, then bump the sequence number so
    // the reader (in another process) always sees a consistent snapshot.
    UccOverlayData tmp = data;
    tmp.sequence = ++m_seq;
    std::memcpy( m_ptr, &tmp, sizeof( UccOverlayData ) );
    __atomic_store_n( &m_ptr->sequence, m_seq, __ATOMIC_RELEASE );
  }

  /// Mark the overlay as inactive (scan finished / cancelled).
  void setInactive()
  {
    if ( !m_ptr )
      return;
    __atomic_store_n( &m_ptr->active, static_cast<uint8_t>( 0 ), __ATOMIC_RELEASE );
    ++m_seq;
    __atomic_store_n( &m_ptr->sequence, m_seq, __ATOMIC_RELEASE );
  }

private:
  OverlayShmWriter() = default;

  ~OverlayShmWriter()
  {
    if ( m_ptr )
    {
      m_ptr->active = 0;
      ::munmap( m_ptr, sizeof( UccOverlayData ) );
    }
    if ( m_fd >= 0 )
      ::close( m_fd );
    // Do NOT shm_unlink — the layer process may still have it mapped.
  }

  OverlayShmWriter( const OverlayShmWriter & ) = delete;
  OverlayShmWriter &operator=( const OverlayShmWriter & ) = delete;

  bool ensureOpen()
  {
    if ( m_ptr )
      return true;
    m_fd = ::shm_open( kUccOverlayShmName, O_CREAT | O_RDWR, 0644 );
    if ( m_fd < 0 )
      return false;
    if ( ::ftruncate( m_fd, sizeof( UccOverlayData ) ) != 0 )
    {
      ::close( m_fd );
      m_fd = -1;
      return false;
    }
    void *p = ::mmap( nullptr, sizeof( UccOverlayData ),
                      PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0 );
    if ( p == MAP_FAILED )
    {
      ::close( m_fd );
      m_fd = -1;
      return false;
    }
    m_ptr = static_cast<UccOverlayData *>( p );
    std::memset( m_ptr, 0, sizeof( UccOverlayData ) );
    return true;
  }

  int              m_fd  = -1;
  UccOverlayData  *m_ptr = nullptr;
  uint32_t         m_seq = 0;
};
