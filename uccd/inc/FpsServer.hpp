// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <sys/types.h>

/**
 * @brief Lightweight Unix-socket server that receives FPS data from the
 *        ucc-fps-layer Vulkan implicit layer (or GLX LD_PRELOAD hook).
 *
 * Protocol (layer → uccd):  "fps:<value>\n"   e.g. "fps:87.3\n"
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
  int    m_clientFd  = -1;   ///< Only one client at a time (one layer process).
  std::string m_socketPath = kSocketPath;
  char   m_buf[ 128 ] = {};
  int    m_bufUsed   = 0;

  double m_currentFps  = -1.0;
  double m_baselineFps = -1.0;
  double m_bestFps     = -1.0;

  pid_t       m_clientPid     = 0;
  std::string m_clientAppName;
};
