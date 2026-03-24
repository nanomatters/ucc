// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <string>
#include <sys/types.h>
#include <vector>

/// Ring buffer of per-frame durations (in microseconds) received via the
/// "ft:" protocol extension.  The daemon pushes each parsed frame time here;
/// consumers (AutoUV/OC workers) call windowSorted() to get a percentile-
/// ready snapshot of recent frame times.
struct FrameTimeBuffer
{
  static constexpr size_t kCapacity = 32768;  // ~30 s @ 1000 fps

  double times[ kCapacity ]{};
  size_t head = 0;
  size_t size = 0;

  void push( double frameTimeUs )
  {
    times[ head % kCapacity ] = frameTimeUs;
    ++head;
    if ( size < kCapacity )
      ++size;
  }

  void clear()
  {
    head = 0;
    size = 0;
  }

  /// Return sorted frame times within the last @p windowUs microseconds.
  /// Walks backward from head, collecting entries until the cumulative
  /// duration exceeds the window.
  [[nodiscard]] std::vector< double > windowSorted( double windowUs ) const
  {
    if ( size == 0 )
      return {};

    std::vector< double > out;
    out.reserve( std::min( size, static_cast< size_t >( 4096 ) ) );
    double cumulative = 0.0;
    for ( size_t i = 0; i < size; ++i )
    {
      const size_t idx = ( head - 1 - i ) % kCapacity;
      const double t = times[ idx ];
      out.push_back( t );
      cumulative += t;
      if ( cumulative >= windowUs )
        break;
    }
    std::sort( out.begin(), out.end() );
    return out;
  }
};

/**
 * @brief Lightweight Unix-socket server that receives FPS data from the
 *        ucc-fps-layer Vulkan implicit layer (or GLX LD_PRELOAD hook).
 *
 * Protocol (layer → uccd):
 *   "fps:<value>\n"                  — aggregated FPS (backward compat)
 *   "ft:<d1>,<d2>,...,<dN>\n"        — per-frame durations in microseconds
 *
 * Lifecycle:
 *   - Call start() before launching / while a game is running.
 *     Creates /run/ucc/uccd-fps.sock; the layer detects the file and connects.
 *     Falls back to /tmp/uccd-fps.sock if /run/ucc is unavailable.
 *   - Call poll() regularly (e.g. from the Auto-OC poll timer) to accept
 *     incoming connections and drain pending data.  Non-blocking; returns
 *     immediately if nothing is pending.
 *   - Call stop() when the scan is done.  Closes the socket and removes the
 *     socket file so the layer stops sending.
 *   - currentFps() returns the most recently parsed value, or -1.0 if no
 *     data has been received yet.
 *   - baselineFps() / setBestFps() track the best FPS seen during the scan
 *     (uccd sets these at the appropriate phases).
 */
class FpsServer
{
public:
  static constexpr const char *kSocketPath = "/run/ucc/uccd-fps.sock";
  static constexpr const char *kLegacySocketPath = "/tmp/uccd-fps.sock";
  /// Abstract socket name (no filesystem path — works inside containers).
  static constexpr const char *kAbstractSocketName = "uccd-fps";

  FpsServer() = default;
  ~FpsServer();

  FpsServer( const FpsServer & ) = delete;
  FpsServer &operator=( const FpsServer & ) = delete;

  /// Create the socket and start listening.  Returns false on error.
  bool start();

  /// Non-blocking: accept pending connections, read pending data, update FPS.
  void poll();

  /// Close the socket and remove the socket file.
  void stop();

  /// Recreate the socket file if it has been removed while the server was
  /// running.  Preserves the reference count.
  void rebind();

  /// @return Most recently received FPS, or -1.0 if no data yet.
  [[nodiscard]] double currentFps() const { return m_currentFps; }

  /// @return True if the server is currently listening.
  [[nodiscard]] bool isRunning() const { return m_listenFd >= 0; }

  /// @return PID of the currently connected client (0 if none).
  [[nodiscard]] pid_t clientPid() const { return m_clientPid; }

  /// @return Name of the connected client process (empty if none).
  [[nodiscard]] const std::string &clientAppName() const { return m_clientAppName; }

  /// @return Active socket path currently used by the server.
  [[nodiscard]] const std::string &socketPath() const { return m_socketPath; }

  /// Snapshot the current FPS as the "baseline" (called at baseline phase end).
  void snapshotBaseline() { m_baselineFps = m_currentFps; }

  /// @return Baseline FPS snapshot (−1 = not yet captured).
  [[nodiscard]] double baselineFps() const { return m_baselineFps; }

  /// @return fps improvement ratio vs baseline, or 0 if unavailable.
  [[nodiscard]] double improvementPct() const;

  /// @return Reference to the per-frame time ring buffer.
  [[nodiscard]] const FrameTimeBuffer &frameTimes() const { return m_frameTimes; }

  /// @return True if the layer is sending per-frame "ft:" data.
  [[nodiscard]] bool hasFrameTimes() const { return m_hasFrameTimes; }

private:
  /// Reference count: start() increments, stop() decrements; the socket is
  /// closed only when the count reaches zero.  Allows monitoring and AutoOC
  /// to share the same server instance without lifecycle conflicts.
  int m_refCount = 0;
  /// Accept all pending clients (non-blocking).
  void acceptClients();

  /// Drain all pending data from m_clientFd; parse "fps:" lines.
  void readClient();

  int    m_listenFd  = -1;
  int    m_abstractListenFd = -1; ///< Abstract socket listener (for container access).
  int    m_clientFd  = -1;   ///< Only one client at a time (one layer process).
  std::string m_socketPath = kSocketPath;
  char   m_buf[ 4096 ] = {};  ///< Read buffer — sized for ft: batches (~800 B at 300 fps).
  int    m_bufUsed   = 0;

  double m_currentFps  = -1.0;
  double m_baselineFps = -1.0;
  double m_bestFps     = -1.0;

  FrameTimeBuffer m_frameTimes;
  bool m_hasFrameTimes = false;

  pid_t       m_clientPid     = 0;
  std::string m_clientAppName;
  std::chrono::steady_clock::time_point m_lastClientActivity{};
  std::chrono::steady_clock::time_point m_clientConnectTime{};
  std::chrono::steady_clock::time_point m_lastPositiveFps{};
};
