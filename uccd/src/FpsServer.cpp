// SPDX-License-Identifier: GPL-3.0-or-later
#include "FpsServer.hpp"

#include <cerrno>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <syslog.h>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

// ── Helpers ──────────────────────────────────────────────────────────────────

static bool set_nonblocking( int fd )
{
  int flags = ::fcntl( fd, F_GETFL, 0 );
  if ( flags < 0 ) return false;
  return ::fcntl( fd, F_SETFL, flags | O_NONBLOCK ) == 0;
}

static bool ensure_dir_exists( const std::filesystem::path &dir )
{
  std::error_code ec;
  if ( std::filesystem::exists( dir, ec ) )
    return std::filesystem::is_directory( dir, ec );
  return std::filesystem::create_directories( dir, ec );
}

// ── FpsServer ────────────────────────────────────────────────────────────────

FpsServer::~FpsServer()
{
  m_refCount = 1;  // force stop() to actually close the socket
  stop();
}

bool FpsServer::start()
{
  ++m_refCount;
  if ( m_listenFd >= 0 )
    return true;  // already running — just increment reference count

  const std::array< const char *, 2 > candidates = { kSocketPath, kLegacySocketPath };
  bool bound = false;

  for ( const char *candidatePath : candidates )
  {
    const std::filesystem::path path( candidatePath );
    const auto parent = path.parent_path();
    if ( !parent.empty() && !ensure_dir_exists( parent ) )
    {
      syslog( LOG_WARNING, "FpsServer: cannot create socket directory '%s'", parent.c_str() );
      continue;
    }

    // Remove stale socket file, if any.
    ::unlink( candidatePath );

    m_listenFd = ::socket( AF_UNIX, SOCK_STREAM, 0 );
    if ( m_listenFd < 0 )
    {
      syslog( LOG_WARNING, "FpsServer: socket() failed: %s", ::strerror( errno ) );
      return false;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    ::strncpy( addr.sun_path, candidatePath, sizeof( addr.sun_path ) - 1 );

    if ( ::bind( m_listenFd, reinterpret_cast<sockaddr *>( &addr ), sizeof( addr ) ) == 0 )
    {
      m_socketPath = candidatePath;
      bound = true;
      break;
    }

    syslog( LOG_WARNING, "FpsServer: bind(%s) failed: %s", candidatePath, ::strerror( errno ) );
    ::close( m_listenFd );
    m_listenFd = -1;
  }

  if ( !bound )
  {
    syslog( LOG_WARNING, "FpsServer: failed to bind any socket path" );
    return false;
  }

  if ( ::listen( m_listenFd, 1 ) != 0 )
  {
    syslog( LOG_WARNING, "FpsServer: listen() failed: %s", ::strerror( errno ) );
    ::close( m_listenFd );
    ::unlink( m_socketPath.c_str() );
    m_listenFd = -1;
    return false;
  }

  // Allow unprivileged game processes to connect to the daemon-owned socket.
  if ( ::chmod( m_socketPath.c_str(), 0666 ) != 0 )
  {
    syslog( LOG_WARNING, "FpsServer: chmod(%s, 0666) failed: %s", m_socketPath.c_str(), ::strerror( errno ) );
  }

  // Remove stale socket on the non-active path to avoid clients connecting to
  // the wrong endpoint if both paths exist.
  const char *inactivePath = ( m_socketPath == kSocketPath ) ? kLegacySocketPath : kSocketPath;
  ::unlink( inactivePath );

  set_nonblocking( m_listenFd );

  m_currentFps  = -1.0;
  m_baselineFps = -1.0;
  m_bestFps     = -1.0;
  m_bufUsed     = 0;
  m_clientPid   = 0;
  m_clientAppName.clear();

  syslog( LOG_INFO, "FpsServer: listening on %s", m_socketPath.c_str() );
  return true;
}

void FpsServer::stop()
{
  if ( m_refCount > 0 && --m_refCount > 0 )
    return;  // still referenced by another caller — keep socket open

  if ( m_clientFd >= 0 )
  {
    ::close( m_clientFd );
    m_clientFd = -1;
  }
  if ( m_listenFd >= 0 )
  {
    ::close( m_listenFd );
    m_listenFd = -1;
    ::unlink( m_socketPath.c_str() );
    syslog( LOG_INFO, "FpsServer: stopped" );
  }

  m_currentFps = -1.0;
  m_bufUsed = 0;
  m_clientPid = 0;
  m_clientAppName.clear();
}

void FpsServer::rebind()
{
  if ( m_listenFd < 0 )
    return;  // not running — nothing to recover

  // Close existing FDs.
  if ( m_clientFd >= 0 ) { ::close( m_clientFd ); m_clientFd = -1; }
  ::close( m_listenFd );
  m_listenFd = -1;
  m_currentFps = -1.0;
  m_bufUsed = 0;
  m_clientPid = 0;
  m_clientAppName.clear();

  // Re-create socket + bind + listen (reuse the existing start() logic but
  // without touching the reference count).
  const int saved = m_refCount;
  m_refCount = 0;
  start();
  m_refCount = saved;
}

void FpsServer::poll()
{
  if ( m_listenFd < 0 )
    return;

  acceptClients();
  if ( m_clientFd >= 0 )
    readClient();
}

void FpsServer::acceptClients()
{
  for ( ;; )
  {
    int fd = ::accept( m_listenFd, nullptr, nullptr );
    if ( fd < 0 )
    {
      if ( errno == EAGAIN || errno == EWOULDBLOCK )
        break;  // no more pending connections
      break;
    }

    // Only one client at a time; drop the old one if a new layer process
    // reconnects (e.g. game restarted).
    if ( m_clientFd >= 0 )
      ::close( m_clientFd );

    set_nonblocking( fd );
    m_clientFd = fd;
    m_bufUsed  = 0;

    // Identify the connected process via SO_PEERCRED.
    m_clientPid = 0;
    m_clientAppName.clear();
    struct ucred cred{};
    socklen_t credLen = sizeof( cred );
    if ( ::getsockopt( fd, SOL_SOCKET, SO_PEERCRED, &cred, &credLen ) == 0 && cred.pid > 0 )
    {
      m_clientPid = cred.pid;
      // Read the executable name from /proc/<pid>/comm (max 16 chars by kernel).
      std::string commPath = "/proc/" + std::to_string( cred.pid ) + "/comm";
      std::ifstream ifs( commPath );
      if ( ifs.is_open() )
      {
        std::getline( ifs, m_clientAppName );
        // Trim trailing whitespace/newline
        while ( !m_clientAppName.empty() &&
                ( m_clientAppName.back() == '\n' || m_clientAppName.back() == '\r' ||
                  m_clientAppName.back() == ' ' ) )
          m_clientAppName.pop_back();
      }
      syslog( LOG_INFO, "FpsServer: client connected — pid=%d app='%s'",
              static_cast< int >( cred.pid ), m_clientAppName.c_str() );
    }
    else
    {
      syslog( LOG_DEBUG, "FpsServer: client connected (unknown PID)" );
    }
  }
}

void FpsServer::readClient()
{
  for ( ;; )
  {
    int space = static_cast<int>( sizeof( m_buf ) ) - m_bufUsed - 1;
    if ( space <= 0 )
    {
      // Buffer full without newline – discard
      m_bufUsed = 0;
      break;
    }

    ssize_t n = ::read( m_clientFd, m_buf + m_bufUsed, static_cast<size_t>( space ) );
    if ( n <= 0 )
    {
      if ( n == 0 || ( errno != EAGAIN && errno != EWOULDBLOCK ) )
      {
        // Connection closed or error
        ::close( m_clientFd );
        m_clientFd = -1;
        m_currentFps = -1.0;
        m_bufUsed = 0;
        m_clientPid = 0;
        m_clientAppName.clear();
        syslog( LOG_DEBUG, "FpsServer: client disconnected" );
      }
      break;
    }

    m_bufUsed += static_cast<int>( n );
    m_buf[ m_bufUsed ] = '\0';

    // Parse complete lines
    char *start = m_buf;
    char *nl;
    while ( ( nl = static_cast<char *>( ::memchr( start, '\n',
                    static_cast<size_t>( m_bufUsed - ( start - m_buf ) ) ) ) ) != nullptr )
    {
      *nl = '\0';

      double fps = -1.0;
      if ( ::sscanf( start, "fps:%lf", &fps ) == 1 && fps >= 0.0 )
      {
        m_currentFps = fps;
        if ( fps > m_bestFps )
          m_bestFps = fps;
      }

      start = nl + 1;
    }

    // Shift remaining partial line to front of buffer
    int remaining = m_bufUsed - static_cast<int>( start - m_buf );
    if ( remaining > 0 && start != m_buf )
      ::memmove( m_buf, start, static_cast<size_t>( remaining ) );
    m_bufUsed = remaining;
  }
}

double FpsServer::improvementPct() const
{
  if ( m_baselineFps <= 0.0 || m_currentFps <= 0.0 )
    return 0.0;
  return ( ( m_currentFps - m_baselineFps ) / m_baselineFps ) * 100.0;
}
