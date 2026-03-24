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

/// Lock-free per-frame timestamp collector.
/// The render thread calls record() on every present; the reporter thread
/// drains the ring buffer each interval and sends batched frame times.
struct FrameTimeCollector
{
  static constexpr size_t kCapacity = 2048;

  std::atomic< uint64_t > timestamps[ kCapacity ]{};
  std::atomic< uint32_t > writeIdx{ 0 };
  std::atomic< uint32_t > count{ 0 };

  void record( uint64_t nowUs )
  {
    const uint32_t idx = writeIdx.fetch_add( 1, std::memory_order_relaxed ) % kCapacity;
    timestamps[ idx ].store( nowUs, std::memory_order_relaxed );
    count.fetch_add( 1, std::memory_order_relaxed );
  }
};

inline FrameTimeCollector g_collector;

inline int try_connect()
{
  // Try abstract socket first (works inside containers).
  {
    const int fd = ::socket( AF_UNIX, SOCK_STREAM, 0 );
    if ( fd >= 0 )
    {
      sockaddr_un addr{};
      addr.sun_family = AF_UNIX;
      const size_t nameLen = std::strlen( kAbstractSocketName );
      std::memcpy( addr.sun_path + 1, kAbstractSocketName, nameLen );
      const socklen_t addrLen = static_cast< socklen_t >(
          offsetof( sockaddr_un, sun_path ) + 1 + nameLen );

      if ( ::connect( fd, reinterpret_cast< sockaddr * >( &addr ), addrLen ) == 0 )
        return fd;
      ::close( fd );
    }
  }

  // Fall back to file-based socket paths.
  for ( const std::string &path : socket_paths() )
  {
    const int fd = ::socket( AF_UNIX, SOCK_STREAM, 0 );
    if ( fd < 0 )
      continue;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy( addr.sun_path, path.c_str(), sizeof( addr.sun_path ) - 1 );

    if ( ::connect( fd, reinterpret_cast< sockaddr * >( &addr ), sizeof( addr ) ) == 0 )
      return fd;
    ::close( fd );
  }

  return -1;
}

inline void fps_reporter_thread()
{
  int sockFd = -1;
  uint32_t lastDrainIdx = 0;
  auto lastTime = std::chrono::steady_clock::now();
  const auto interval = std::chrono::milliseconds( 500 );

  while ( true )
  {
    std::this_thread::sleep_for( interval );

    // ── Drain frame timestamps ──────────────────────────────────────────
    const auto now = std::chrono::steady_clock::now();
    const uint32_t curWriteIdx = g_collector.writeIdx.load( std::memory_order_relaxed );
    const uint32_t frameCount = curWriteIdx - lastDrainIdx;
    const double dt = std::chrono::duration< double >( now - lastTime ).count();
    const double fps = ( dt > 0.0 && frameCount > 0 )
      ? static_cast< double >( frameCount ) / dt : 0.0;

    // Collect timestamps, compute inter-frame deltas (μs)
    std::vector< uint32_t > deltas;
    if ( frameCount > 0 && frameCount <= FrameTimeCollector::kCapacity )
    {
      deltas.reserve( frameCount );
      // Read timestamps in order and compute deltas
      std::vector< uint64_t > ts;
      ts.reserve( frameCount );
      for ( uint32_t i = lastDrainIdx; i < curWriteIdx; ++i )
      {
        const uint32_t idx = i % FrameTimeCollector::kCapacity;
        ts.push_back( g_collector.timestamps[ idx ].load( std::memory_order_relaxed ) );
      }
      for ( size_t i = 1; i < ts.size(); ++i )
      {
        const int64_t d = static_cast< int64_t >( ts[ i ] ) - static_cast< int64_t >( ts[ i - 1 ] );
        if ( d > 0 && d < 1000000 )  // sanity: >0 and <1s
          deltas.push_back( static_cast< uint32_t >( d ) );
      }
    }

    lastDrainIdx = curWriteIdx;
    lastTime = now;

    // ── Connect ─────────────────────────────────────────────────────────
    if ( sockFd < 0 )
      sockFd = try_connect();

    if ( sockFd < 0 )
      continue;

    // ── Send ────────────────────────────────────────────────────────────
    if ( frameCount == 0 )
    {
      static constexpr const char *kKeepalive = "fps:0.0\n";
      if ( ::write( sockFd, kKeepalive, std::strlen( kKeepalive ) ) < 0 )
      {
        ::close( sockFd );
        sockFd = -1;
      }
      continue;
    }

    // Build fps: line (backward compat) + ft: line (frame times in μs)
    std::string msg;
    msg.reserve( 32 + deltas.size() * 7 );

    char fpsBuf[ 32 ];
    const int fpsLen = std::snprintf( fpsBuf, sizeof( fpsBuf ), "fps:%.1f\n", fps );
    if ( fpsLen > 0 )
      msg.append( fpsBuf, static_cast< size_t >( fpsLen ) );

    if ( !deltas.empty() )
    {
      msg.append( "ft:" );
      for ( size_t i = 0; i < deltas.size(); ++i )
      {
        if ( i > 0 )
          msg.push_back( ',' );
        char dtBuf[ 16 ];
        const int dtLen = std::snprintf( dtBuf, sizeof( dtBuf ), "%u", deltas[ i ] );
        if ( dtLen > 0 )
          msg.append( dtBuf, static_cast< size_t >( dtLen ) );
      }
      msg.push_back( '\n' );
    }

    if ( ::write( sockFd, msg.data(), msg.size() ) < 0 )
    {
      ::close( sockFd );
      sockFd = -1;
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
  const auto now = std::chrono::steady_clock::now();
  const uint64_t nowUs = static_cast< uint64_t >(
    std::chrono::duration_cast< std::chrono::microseconds >( now.time_since_epoch() ).count() );
  g_collector.record( nowUs );
  ensure_reporter_started();
}

}  // namespace uccfps