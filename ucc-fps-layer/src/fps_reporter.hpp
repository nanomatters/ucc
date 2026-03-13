// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace uccfps {

inline constexpr const char *kSocketPathPrimary = "/run/ucc/uccd-fps.sock";
inline constexpr const char *kSocketPathLegacy  = "/tmp/uccd-fps.sock";
/// Abstract socket name — works inside Steam pressure-vessel containers
/// (no filesystem path required, shared via the network namespace).
inline constexpr const char *kAbstractSocketName = "uccd-fps";

inline std::vector< std::string > socket_paths()
{
  std::vector< std::string > paths;
  if ( const char *envPath = ::getenv( "UCC_FPS_SOCKET" ); envPath && envPath[ 0 ] != '\0' )
    paths.emplace_back( envPath );
  paths.emplace_back( kSocketPathPrimary );
  paths.emplace_back( kSocketPathLegacy );
  return paths;
}

inline bool hook_enabled()
{
  static const bool enabled = []() {
    const char *value = ::getenv( "UCC_FPS_HOOK" );
    return value && std::strcmp( value, "1" ) == 0;
  }();
  return enabled;
}

// MangoHud-style process-name blacklist for known launcher/compositor helpers
// that are unstable or unnecessary under the hooking layer.
namespace detail {

inline std::string get_exe_path()
{
  char path[ 4096 ]{};
  const ssize_t n = ::readlink( "/proc/self/exe", path, sizeof( path ) - 1 );
  if ( n <= 0 )
    return {};
  path[ n ] = '\0';
  return path;
}

inline std::string get_basename( const std::string &path )
{
  const auto pos = path.rfind( '/' );
  return pos == std::string::npos ? path : path.substr( pos + 1 );
}

inline bool check_blacklisted()
{
  const std::string name = get_basename( get_exe_path() );
  if ( name.empty() )
    return false;

  // Keep this list in sync with MangoHud's blacklist.cpp (Linux-native entries).
  static const std::vector< std::string > kBlacklist = {
    "Amazon Games UI.exe",
    "Battle.net.exe",
    "BethesdaNetLauncher.exe",
    "EADesktop.exe",
    "EALauncher.exe",
    "EpicGamesLauncher.exe",
    "EpicWebHelper.exe",
    "explorer.exe",
    "ffxivlauncher.exe",
    "ffxivlauncher64.exe",
    "GalaxyClient.exe",
    "FurMark_GUI",
    "gamescope",
    "GardenGate_Launcher.exe",
    "gldriverquery",
    "halloy",
    "IGOProxy.exe",
    "IGOProxy64.exe",
    "iexplore.exe",
    "InsurgencyEAC.exe",
    "Launcher",
    "LeagueClient.exe",
    "LeagueClientUxRender.exe",
    "MarneLauncher.exe",
    "MarvelRivals_Launcher.exe",
    "monado-service",
    "Origin.exe",
    "OriginThinSetupInternal.exe",
    "plutonium.exe",
    "plutonium-launcher-win32.exe",
    "REDlauncher.exe",
    "REDprelauncher.exe",
    "RSI Launcher.exe",
    "rundll32.exe",
    "SocialClubHelper.exe",
    "StarCitizen_Launcher.exe",
    "steam",
    "Steam.exe",
    "steamwebhelper",
    "steamwebhelper.exe",
    "tabtip.exe",
    "UplayWebCore.exe",
    "vrcompositor",
    "vulkandriverquery",
  };

  for ( const auto &entry : kBlacklist )
  {
    if ( name == entry )
      return true;
  }
  return false;
}

}  // namespace detail

inline bool is_blacklisted()
{
  static const bool blacklisted = detail::check_blacklisted();
  return blacklisted;
}

inline std::atomic<uint64_t> g_frame_count{ 0 };

inline void fps_reporter_thread()
{
  int sockFd = -1;
  uint64_t lastCount = 0;
  auto lastTime = std::chrono::steady_clock::now();
  const auto interval = std::chrono::milliseconds( 500 );

  while ( true )
  {
    std::this_thread::sleep_for( interval );

    const auto now = std::chrono::steady_clock::now();
    const uint64_t count = g_frame_count.load( std::memory_order_relaxed );
    const double dt = std::chrono::duration<double>( now - lastTime ).count();
    const double fps = dt > 0.0 ? ( count - lastCount ) / dt : 0.0;
    lastCount = count;
    lastTime = now;

    if ( sockFd < 0 )
    {
      // Try abstract socket first (works inside containers).
      {
        const int fd = ::socket( AF_UNIX, SOCK_STREAM, 0 );
        if ( fd >= 0 )
        {
          sockaddr_un addr{};
          addr.sun_family = AF_UNIX;
          // sun_path[0] = '\0' marks this as abstract (already zero from {}).
          const size_t nameLen = std::strlen( kAbstractSocketName );
          std::memcpy( addr.sun_path + 1, kAbstractSocketName, nameLen );
          const socklen_t addrLen = static_cast< socklen_t >(
              offsetof( sockaddr_un, sun_path ) + 1 + nameLen );

          if ( ::connect( fd, reinterpret_cast< sockaddr * >( &addr ), addrLen ) == 0 )
            sockFd = fd;
          else
            ::close( fd );
        }
      }

      // Fall back to file-based socket paths.
      if ( sockFd < 0 )
      {
      for ( const std::string &path : socket_paths() )
      {
        const int fd = ::socket( AF_UNIX, SOCK_STREAM, 0 );
        if ( fd < 0 )
          continue;

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy( addr.sun_path, path.c_str(), sizeof( addr.sun_path ) - 1 );

        if ( ::connect( fd, reinterpret_cast<sockaddr *>( &addr ), sizeof( addr ) ) == 0 )
        {
          sockFd = fd;
          break;
        }

        ::close( fd );
      }
      }
    }

    if ( sockFd >= 0 )
    {
      if ( count == 0 )
      {
        // Keep the socket session alive so uccd can track the active process,
        // and publish 0.0 FPS until the first frame is observed.
        static constexpr const char *kKeepalive = "fps:0.0\n";
        if ( ::write( sockFd, kKeepalive, std::strlen( kKeepalive ) ) < 0 )
        {
          ::close( sockFd );
          sockFd = -1;
        }
        continue;
      }

      char buf[ 32 ];
      const int n = std::snprintf( buf, sizeof( buf ), "fps:%.1f\n", fps );
      if ( n > 0 && ::write( sockFd, buf, static_cast<size_t>( n ) ) < 0 )
      {
        ::close( sockFd );
        sockFd = -1;
      }
    }
  }
}

inline void ensure_reporter_started()
{
  if ( !hook_enabled() )
    return;

  static std::once_flag started;
  std::call_once( started, []() {
    std::thread t( fps_reporter_thread );
    t.detach();
  } );
}

inline void record_frame()
{
  if ( !hook_enabled() )
    return;
  g_frame_count.fetch_add( 1, std::memory_order_relaxed );
  ensure_reporter_started();
}

}  // namespace uccfps