// SPDX-License-Identifier: GPL-3.0-or-later
#include "FpsServer.hpp"

#include <cerrno>
#include <array>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_set>
#include <syslog.h>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <signal.h>

// ── Helpers ──────────────────────────────────────────────────────────────────

static bool set_nonblocking( int fd )
{
  int flags = ::fcntl( fd, F_GETFL, 0 );
  if ( flags < 0 )
    return false;
  return ::fcntl( fd, F_SETFL, flags | O_NONBLOCK ) == 0;
}

static bool ensure_dir_exists( const std::filesystem::path &dir )
{
  std::error_code ec;
  if ( std::filesystem::exists( dir, ec ) )
    return std::filesystem::is_directory( dir, ec );
  return std::filesystem::create_directories( dir, ec );
}

static std::string process_comm_for_pid( pid_t pid )
{
  if ( pid <= 0 )
    return {};

  std::string commPath = "/proc/" + std::to_string( pid ) + "/comm";
  std::ifstream ifs( commPath );
  if ( !ifs.is_open() )
    return {};

  std::string comm;
  std::getline( ifs, comm );
  while ( !comm.empty() &&
          ( comm.back() == '\n' || comm.back() == '\r' || comm.back() == ' ' ) )
    comm.pop_back();
  return comm;
}

static std::string process_exe_basename_for_pid( pid_t pid )
{
  if ( pid <= 0 )
    return {};

  std::array< char, 512 > path{};
  const std::string exePath = "/proc/" + std::to_string( pid ) + "/exe";
  const ssize_t n = ::readlink( exePath.c_str(), path.data(), path.size() - 1 );
  if ( n <= 0 )
    return {};

  path[ static_cast<size_t>( n ) ] = '\0';
  const char *base = ::strrchr( path.data(), '/' );
  return base ? std::string( base + 1 ) : std::string( path.data() );
}

static std::string process_name_for_pid( pid_t pid )
{
  std::string name = process_comm_for_pid( pid );
  if ( !name.empty() )
    return name;
  return process_exe_basename_for_pid( pid );
}

static std::string to_lower_copy( const std::string &input )
{
  std::string out = input;
  std::transform( out.begin(), out.end(), out.begin(),
                  []( unsigned char c ) { return static_cast< char >( std::tolower( c ) ); } );
  return out;
}

static bool is_blacklisted_fps_process( const std::string &name )
{
  if ( name.empty() )
    return false;

  // Keep this list aligned with MangoHud's launcher/helper blacklist,
  // plus Wine/Proton helpers that load Vulkan but never render.
  static const std::unordered_set< std::string > kBlacklist = {
    "amazongamesui.exe",
    "battle.net.exe",
    "bethesdanetlauncher.exe",
    "cs2.sh",
    "eadesktop.exe",
    "ealauncher.exe",
    "epicgameslauncher.exe",
    "epicwebhelper.exe",
    "explorer.exe",
    "ffxivlauncher.exe",
    "ffxivlauncher64.exe",
    "galaxyclient.exe",
    "gamescope",
    "gardengate_launcher.exe",
    "gldriverquery",
    "halloy",
    "igoproxy.exe",
    "igoproxy64.exe",
    "iexplore.exe",
    "insurgencyeac.exe",
    "launcher",
    "leagueclient.exe",
    "leagueclientuxrender.exe",
    "marnelauncher.exe",
    "marvelrivals_launcher.exe",
    "monado-service",
    "origin.exe",
    "originthinsetupinternal.exe",
    "plutonium.exe",
    "plutonium-launcher-win32.exe",
    "redlauncher.exe",
    "redprelauncher.exe",
    "rsi launcher.exe",
    "rundll32.exe",
    "socialclubhelper.exe",
    "starcitizen_launcher.exe",
    "steam",
    "steam.exe",
    "steamwebhelper",
    "steamwebhelper.exe",
    "tabtip.exe",
    "uplaywebcore.exe",
    "vrcompositor",
    "vulkandriverquery",
    // Wine/Proton helpers that load Vulkan but never render frames.
    "winedevice.exe",
    "xalia.exe",
  };

  return kBlacklist.find( to_lower_copy( name ) ) != kBlacklist.end();
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

  // ── Abstract socket (MangoHud-style) ───────────────────────────────────
  // Abstract Unix sockets live in the kernel's network namespace, not in any
  // filesystem.  This makes them accessible from inside Steam pressure-vessel
  // containers which mount-isolate /run and /tmp.
  {
    const int afd = ::socket( AF_UNIX, SOCK_STREAM, 0 );
    if ( afd >= 0 )
    {
      sockaddr_un aaddr{};
      aaddr.sun_family = AF_UNIX;
      // sun_path[0] = '\0' marks this as an abstract socket.
      const size_t nameLen = std::strlen( kAbstractSocketName );
      std::memcpy( aaddr.sun_path + 1, kAbstractSocketName, nameLen );
      const socklen_t addrLen = static_cast< socklen_t >(
          offsetof( sockaddr_un, sun_path ) + 1 + nameLen );

      if ( ::bind( afd, reinterpret_cast< sockaddr * >( &aaddr ), addrLen ) == 0
           && ::listen( afd, 4 ) == 0 )
      {
        set_nonblocking( afd );
        m_abstractListenFd = afd;
        syslog( LOG_INFO, "FpsServer: abstract socket '@%s' ready", kAbstractSocketName );
      }
      else
      {
        syslog( LOG_WARNING, "FpsServer: abstract socket '@%s' failed: %s",
                kAbstractSocketName, ::strerror( errno ) );
        ::close( afd );
      }
    }
  }

  m_currentFps  = -1.0;
  m_baselineFps = -1.0;
  m_bestFps     = -1.0;
  m_bufUsed     = 0;
  m_clientPid   = 0;
  m_clientAppName.clear();
  m_lastClientActivity = {};
  m_clientConnectTime = {};
  m_lastPositiveFps = {};

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
  if ( m_abstractListenFd >= 0 )
  {
    ::close( m_abstractListenFd );
    m_abstractListenFd = -1;
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
  m_lastClientActivity = {};
  m_clientConnectTime = {};
  m_lastPositiveFps = {};
}

void FpsServer::rebind()
{
  if ( m_listenFd < 0 )
    return;  // not running — nothing to recover

  // Close existing FDs.
  if ( m_clientFd >= 0 ) { ::close( m_clientFd ); m_clientFd = -1; }
  if ( m_abstractListenFd >= 0 ) { ::close( m_abstractListenFd ); m_abstractListenFd = -1; }
  ::close( m_listenFd );
  m_listenFd = -1;
  m_currentFps = -1.0;
  m_bufUsed = 0;
  m_clientPid = 0;
  m_clientAppName.clear();
  m_lastClientActivity = {};
  m_clientConnectTime = {};
  m_lastPositiveFps = {};

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

  static constexpr auto kIdleTimeout = std::chrono::seconds( 5 );

  // Read from the current client BEFORE accepting new ones, so the current
  // client can establish positive-FPS proof before newcomers are evaluated.
  if ( m_clientFd >= 0 )
    readClient();

  acceptClients();

  // Proactively detect dead clients via PID liveness check.
  // If the tracked PID no longer exists (e.g. game exited but an orphaned
  // child inherited the socket FD and kept it open), forcibly close the
  // connection rather than waiting indefinitely for socket EOF.
  if ( m_clientFd >= 0 && m_clientPid > 0 )
  {
    if ( ::kill( m_clientPid, 0 ) == -1 && errno == ESRCH )
    {
      syslog( LOG_INFO, "FpsServer: client PID %d no longer exists — closing stale connection",
              static_cast< int >( m_clientPid ) );
      ::close( m_clientFd );
      m_clientFd = -1;
      m_currentFps = -1.0;
      m_bufUsed = 0;
      m_clientPid = 0;
      m_clientAppName.clear();
      m_lastClientActivity = {};
      m_clientConnectTime = {};
      m_lastPositiveFps = {};
    }
  }

  // Handle PID reuse and long-lived stale sockets that stop sending FPS.
  if ( m_clientFd >= 0 && m_clientPid > 0 )
  {
    const std::string currentComm = process_comm_for_pid( m_clientPid );
    if ( !m_clientAppName.empty() && !currentComm.empty() && currentComm != m_clientAppName )
    {
      syslog( LOG_INFO,
              "FpsServer: PID %d changed process name '%s' -> '%s' — closing stale connection",
              static_cast< int >( m_clientPid ),
              m_clientAppName.c_str(),
              currentComm.c_str() );
      ::close( m_clientFd );
      m_clientFd = -1;
      m_currentFps = -1.0;
      m_bufUsed = 0;
      m_clientPid = 0;
      m_clientAppName.clear();
      m_lastClientActivity = {};
      m_clientConnectTime = {};
      m_lastPositiveFps = {};
    }
  }

  if ( m_clientFd >= 0 && m_lastClientActivity.time_since_epoch().count() > 0 )
  {
    const auto now = std::chrono::steady_clock::now();
    if ( now - m_lastClientActivity > kIdleTimeout )
    {
      syslog( LOG_INFO,
              "FpsServer: client '%s' (pid=%d) idle for >%llds — closing connection",
              m_clientAppName.c_str(),
              static_cast< int >( m_clientPid ),
              static_cast< long long >( std::chrono::duration_cast< std::chrono::seconds >( kIdleTimeout ).count() ) );
      ::close( m_clientFd );
      m_clientFd = -1;
      m_currentFps = -1.0;
      m_bufUsed = 0;
      m_clientPid = 0;
      m_clientAppName.clear();
      m_lastClientActivity = {};
      m_clientConnectTime = {};
      m_lastPositiveFps = {};
    }
  }
}

void FpsServer::acceptClients()
{
  // Accept from both the file-based and abstract listeners.
  const int fds[] = { m_listenFd, m_abstractListenFd };

  for ( int listenFd : fds )
  {
  if ( listenFd < 0 )
    continue;

  for ( ;; )
  {
    int fd = ::accept( listenFd, nullptr, nullptr );
    if ( fd < 0 )
    {
      if ( errno == EAGAIN || errno == EWOULDBLOCK )
        break;  // no more pending connections
      break;
    }

    // Identify the connected process via SO_PEERCRED.
    pid_t newClientPid = 0;
    std::string newClientAppName;
    struct ucred cred{};
    socklen_t credLen = sizeof( cred );
    if ( ::getsockopt( fd, SOL_SOCKET, SO_PEERCRED, &cred, &credLen ) == 0 && cred.pid > 0 )
    {
      newClientPid = cred.pid;
      newClientAppName = process_name_for_pid( cred.pid );
      if ( newClientAppName.empty() )
      {
        syslog( LOG_DEBUG,
                "FpsServer: ignoring FPS client pid=%d (unable to resolve process name)",
                static_cast< int >( newClientPid ) );
        ::close( fd );
        continue;
      }
      if ( is_blacklisted_fps_process( newClientAppName ) )
      {
        syslog( LOG_DEBUG,
                "FpsServer: ignoring blacklisted FPS client pid=%d app='%s'",
                static_cast< int >( newClientPid ), newClientAppName.c_str() );
        ::close( fd );
        continue;
      }

      // MangoHud-style focus behavior adapted for the daemon socket model:
      // keep ownership while the current client is still actively rendering,
      // OR while the current client is freshly connected and hasn't had time
      // to prove itself yet (grace period).
      if ( m_clientFd >= 0 && m_clientPid > 0 )
      {
        const bool currentAlive = ( ::kill( m_clientPid, 0 ) == 0 ) || ( errno == EPERM );
        const auto now = std::chrono::steady_clock::now();
        const bool currentRecentlyRendering = m_lastPositiveFps.time_since_epoch().count() > 0
                                         && ( now - m_lastPositiveFps ) <= std::chrono::milliseconds( 1500 );
        // Grace period: don't replace a client within 2s of its connection,
        // so it has time to send FPS data and prove it's a renderer.
        const bool currentFresh = m_clientConnectTime.time_since_epoch().count() > 0
                               && ( now - m_clientConnectTime ) <= std::chrono::seconds( 2 );
        if ( currentAlive && ( currentRecentlyRendering || currentFresh ) && newClientPid != m_clientPid )
        {
          syslog( LOG_DEBUG,
                  "FpsServer: keeping active rendering client pid=%d app='%s', ignoring newcomer pid=%d app='%s'",
                  static_cast< int >( m_clientPid ),
                  m_clientAppName.c_str(),
                  static_cast< int >( newClientPid ),
                  newClientAppName.c_str() );
          ::close( fd );
          continue;
        }
      }

      // Only one client at a time; replace old one only when needed
      // (e.g. reconnect of same PID or dead/stale ownership).
      if ( m_clientFd >= 0 )
        ::close( m_clientFd );

      set_nonblocking( fd );
      m_clientFd = fd;
      m_bufUsed  = 0;
      m_lastClientActivity = std::chrono::steady_clock::now();
      m_clientConnectTime = m_lastClientActivity;
      m_clientPid = newClientPid;
      m_clientAppName = newClientAppName;
      m_lastPositiveFps = {};

      syslog( LOG_INFO, "FpsServer: client connected — pid=%d app='%s'",
              static_cast< int >( newClientPid ), m_clientAppName.c_str() );
    }
    else
    {
      ::close( fd );
      syslog( LOG_DEBUG, "FpsServer: ignoring FPS client (unknown PID)" );
      continue;
    }
  }
  } // for each listener fd
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
        m_lastClientActivity = {};
        m_clientConnectTime = {};
        m_lastPositiveFps = {};
        syslog( LOG_DEBUG, "FpsServer: client disconnected" );
      }
      break;
    }

    m_lastClientActivity = std::chrono::steady_clock::now();
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
        if ( fps > 1.0 )
          m_lastPositiveFps = std::chrono::steady_clock::now();
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
