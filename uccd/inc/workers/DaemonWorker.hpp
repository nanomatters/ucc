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

#include <chrono>
#include <atomic>
#include <syslog.h>
#include <cstdio>
#include <QThread>
#include <typeinfo>

namespace ucc {
  // Worker debug flag: when true, worker classes will emit debug messages.
  inline std::atomic_bool g_worker_debug{false};

  inline void setWorkerDebug(bool enabled) { g_worker_debug.store(enabled); }

  template<typename... Args>
  inline void wDebug(const char *fmt, Args&&... args)
  {
    if ( not g_worker_debug.load() )
      return;

    // format into a stack buffer (truncate if necessary)
    char buf[1024];
    // snprintf with a non-literal format may trigger -Wformat-nonliteral; silence locally
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
    int n = std::snprintf(buf, sizeof(buf), fmt, std::forward<Args>(args)...);
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
    if ( n >= 0 )
      syslog(LOG_DEBUG, "%s", buf);
  }
  
  template<typename Func>
  inline void invokeBlocking(QObject *receiver, Func &&fn)
  {
    if ( receiver == nullptr )
      return;

    if ( receiver->thread() == QThread::currentThread() )
    {
      // Same thread - call directly to avoid Qt dead lock detection
      fn();
      return;
    }

    // Different thread - use BlockingQueuedConnection and let Qt handle synchronization
    QMetaObject::invokeMethod(receiver, std::forward<Func>(fn), Qt::BlockingQueuedConnection);
  }
}

/**
 * @brief Abstract base class for daemon worker threads
 *
 * Provides a framework for periodic work execution with automatic timer management.
 * Subclasses must implement onStart(), onWork(), and onExit() lifecycle methods.
 *
 * The worker automatically creates a background thread that:
 *   - Calls onStart() once at initialization
 *   - Calls onWork() repeatedly at the specified timeout interval
 *   - Calls onExit() during cleanup
 *
 * Uses QThread for proper Qt object threading support.
 *
 * Usage:
 *   - Create concrete subclass inheriting from DaemonWorker
 *   - Implement pure virtual methods: onStart(), onWork(), onExit()
 *   - Constructor automatically starts the timer thread
 *   - Call stop() to gracefully shutdown the worker
 */
class DaemonWorker : public QThread
{
  Q_OBJECT

public:
  /**
   * @brief Constructor
   *
   * Creates the worker and automatically starts the timer thread if autoStart is true.
   * The thread will:
   *   1. Call onStart() once on startup
   *   2. Repeatedly call onWork() at timeout intervals
   *   3. Call onExit() when the thread stops
   *
   * @param timeout Duration in milliseconds between work cycles
   * @param autoStart Whether to automatically start the worker thread
   */
  explicit DaemonWorker( std::chrono::milliseconds timeout, bool autoStart = true )
    : m_timeout( timeout ), m_isRunning( false )
  {
    if ( autoStart )
      start();
  }

  /**
   * @brief Virtual destructor
   *
   * Automatically stops the timer thread.
   * Note: onExit() is NOT called when the thread stops due to destruction,
   * because the derived class vtable is no longer valid at this point.
   * Derived classes that need cleanup should call stop() in their own destructor.
   */
  virtual ~DaemonWorker() noexcept
  {
    m_destroying = true;
    stop();
  }

  // Prevent copy and move operations for this base class
  DaemonWorker( const DaemonWorker & ) = delete;
  DaemonWorker( DaemonWorker && ) = delete;
  DaemonWorker &operator=( const DaemonWorker & ) = delete;
  DaemonWorker &operator=( DaemonWorker && ) = delete;

  /**
   * @brief Gracefully stop the worker thread
   */
  void stop()
  {
    if ( m_isRunning )
    {
      m_isRunning = false;
      QThread::wait();
    }
  }

  /**
   * @brief Start the worker thread
   */
  void start()
  {
    if ( m_isRunning )
      return;

    m_isRunning = true;
    QThread::start();
  }

  /**
   * @brief Get the timeout duration
   * @return The timeout in milliseconds
   */
  [[nodiscard]] std::chrono::milliseconds getTimeout() const noexcept
  {
    return m_timeout;
  }

  /**
   * @brief Check if the worker is running
   * @return True if the worker thread is active
   */
  [[nodiscard]] bool isRunning() const noexcept
  {
    return m_isRunning;
  }

protected:
  /**
   * @brief QThread run method override
   */
  void run() override
  {
    try
    {
      ucc::wDebug("[DEBUG] DaemonWorker: starting %s on thread %p", typeid(*this).name(), reinterpret_cast<void*>(QThread::currentThreadId()) );
      onStart();

      bool firstLoop = true;
      while ( m_isRunning )
      {
        if ( firstLoop ) {
          ucc::wDebug("[DEBUG] DaemonWorker: first onWork for %s", typeid(*this).name());
          firstLoop = false;
        }
        onWork();
        QThread::msleep( m_timeout.count() );
      }

      ucc::wDebug("[DEBUG] DaemonWorker: exiting %s", typeid(*this).name());
      if ( !m_destroying )
        onExit();
    }
    catch ( const std::exception & )
    {
      // silently ignore exceptions to prevent thread from crashing
    }
  }
  /**
   * @brief Called once when the worker starts
   *
   * Invoked by the timer thread on startup, before the periodic work loop begins.
   * Must be implemented by derived classes.
   */
  virtual void onStart() = 0;

  /**
   * @brief Called repeatedly during the work cycle
   *
   * Invoked by the timer thread at each timeout interval.
   * Must be implemented by derived classes.
   */
  virtual void onWork() = 0;

  /**
   * @brief Called when the worker exits
   *
   * Invoked by the timer thread when stop() is called.
   * Must be implemented by derived classes.
   */
  virtual void onExit() = 0;

private:
  const std::chrono::milliseconds m_timeout;
  std::atomic< bool > m_isRunning;
  std::atomic< bool > m_destroying { false };
};
