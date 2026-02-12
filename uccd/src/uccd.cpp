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

#include <iostream>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <cstring>
#include <cerrno>
#include <signal.h>
#include <syslog.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <filesystem>
#include <cstdlib>

// Qt includes
#include <QCoreApplication>
#include <QSocketNotifier>
#include <QTimer>

// Hardware abstraction layer
#include "tuxedo_io_lib/tuxedo_io_api.hh"
#include "workers/HardwareMonitorWorker.hpp"
#include "UccDBusService.hpp"
#include "SettingsManager.hpp"

// C++20 features used
constexpr std::string_view VERSION = "1.0.0-ng";
constexpr std::string_view DAEMON_NAME = "uccd";
constexpr std::string_view PID_FILE = "/run/uccd.pid";

// Modern, signal-safe shutdown using a pipe + QSocketNotifier.
// Signal handlers write a byte into a pipe; the Qt event loop reads it
// and performs a safe shutdown (calls `app.quit()`).
static int sig_pipe_fds[2] = { -1, -1 };

static void signal_fd_handler( int sig )
{
  char c = static_cast<char>( sig );
  if ( sig_pipe_fds[1] != -1 )
  {
    // write is async-signal-safe
    ssize_t r = write( sig_pipe_fds[1], &c, 1 );
    (void)r;
  }
}

// Initialize syslog
void init_syslog()
{
  openlog( DAEMON_NAME.data(), LOG_PID, LOG_DAEMON );
  syslog( LOG_INFO, "TuxedoControlCenterDaemon-NG (uccd) starting - version %s", VERSION.data() );
}

// Cleanup syslog
void cleanup_syslog()
{
  syslog( LOG_INFO, "TuxedoControlCenterDaemon-NG (uccd) shutting down" );
  closelog();
}

// PID file management
void write_pid_file()
{
  std::ofstream pidfile( PID_FILE.data() );
  if ( pidfile.is_open() )
  {
    pidfile << getpid();
    pidfile.close();
    syslog( LOG_INFO, "PID file written: %s (pid=%d)", PID_FILE.data(), getpid() );
  }
  else
  {
    syslog( LOG_WARNING, "Failed to write PID file: %s", PID_FILE.data() );
  }
}

void remove_pid_file()
{
  try
  {
    std::filesystem::remove( PID_FILE );
  }
  catch ( const std::exception& e )
  {
    syslog( LOG_WARNING, "Failed to remove PID file: %s", e.what() );
  }
}

// Create /etc/ucc directory if it doesn't exist
void ensure_config_directory()
{
  constexpr std::string_view config_dir = "/etc/ucc";
  
  try
  {
    namespace fs = std::filesystem;
    
    if ( !fs::exists( config_dir ) )
    {
      if ( fs::create_directories( config_dir ) )
      {
        syslog( LOG_INFO, "Created configuration directory: %s", config_dir.data() );
        // Set permissions to 755 (rwxr-xr-x)
        fs::permissions( config_dir, fs::perms::owner_all | fs::perms::group_read | 
                         fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec );
      }
    }
    else
    {
      syslog( LOG_DEBUG, "Configuration directory already exists: %s", config_dir.data() );
    }
  }
  catch ( const std::exception& e )
  {
    syslog( LOG_WARNING, "Failed to ensure configuration directory: %s", e.what() );
  }
}

// Check if another instance is already running
int check_single_instance()
{
  std::ifstream pidfile( PID_FILE.data() );
  if ( pidfile.is_open() )
  {
    pid_t existing_pid = 0;
    pidfile >> existing_pid;
    pidfile.close();

    if ( existing_pid > 0 && kill( existing_pid, 0 ) == 0 )
    {
      std::cerr << "Error: Another instance of the daemon is already running (PID " 
                << existing_pid << ")." << std::endl;
      return 1;
    }

    // Stale PID file, try to clean it up
    try
    {
      std::filesystem::remove( PID_FILE );
    }
    catch ( const std::exception& e )
    {
      // Ignore cleanup errors
    }
  }

  return 0;
}

// Main daemon loop
int run_daemon()
{
  // Create Qt application for event loop (needed for Qt Bluetooth)
  int argc = 1;
  char* argv[] = {const_cast<char*>("uccd")};
  QCoreApplication app(argc, argv);

  init_syslog();

  // Set up signal pipe and Qt notifier so shutdown happens in the Qt event loop.
  if ( pipe( sig_pipe_fds ) == -1 )
  {
    syslog( LOG_ERR, "Failed to create signal pipe: %s", strerror( errno ) );
  }
  else
  {
    // Make read end non-blocking/close-on-exec where appropriate
    fcntl( sig_pipe_fds[0], F_SETFD, FD_CLOEXEC );
    fcntl( sig_pipe_fds[1], F_SETFD, FD_CLOEXEC );

    struct sigaction sa;
    memset( &sa, 0, sizeof( sa ) );
    sa.sa_handler = signal_fd_handler;
    sigemptyset( &sa.sa_mask );
    sa.sa_flags = SA_RESTART;
    sigaction( SIGTERM, &sa, nullptr );
    sigaction( SIGINT, &sa, nullptr );
    sigaction( SIGHUP, &sa, nullptr );
  }

  syslog( LOG_INFO, "Daemon initialized successfully" );

  // Ensure configuration directory exists
  ensure_config_directory();

  // Initialize hardware interface
  try
  {
    TuxedoIOAPI io;
    syslog( LOG_INFO, "Hardware interface initialized" );

    // Detect device capabilities
    bool identified = false;
    if ( io.identify( identified ) and identified )
    {
      std::string interface_id, model_id;
      if ( io.deviceInterfaceIdStr( interface_id ) )
      {
        syslog( LOG_INFO, "Detected interface: %s", interface_id.c_str() );
      }
      if ( io.deviceModelIdStr( model_id ) )
      {
        syslog( LOG_INFO, "Detected model: %s", model_id.c_str() );
      }
    }
    else
    {
      syslog( LOG_WARNING, "No compatible hardware device detected" );
    }

    // Initialize DBus service
    UccDBusService dbusService;
    if ( !dbusService.initDBus() )
    {
      syslog( LOG_ERR, "Failed to initialize D-Bus service" );
      cleanup_syslog();
      return 1;
    }
    dbusService.start();
    syslog( LOG_INFO, "DBus service initialized" );

    // Write PID file
    write_pid_file();

    // Create a QSocketNotifier to watch for signals written into the pipe
    QSocketNotifier* notifier = nullptr;
    if ( sig_pipe_fds[0] != -1 )
    {
      notifier = new QSocketNotifier( sig_pipe_fds[0], QSocketNotifier::Read );
      QObject::connect( notifier, &QSocketNotifier::activated, [&app]( int fd ) {
        // Drain the pipe and then request Qt to quit (safe context)
        char buf[64];
        while ( ::read( fd, buf, sizeof( buf ) ) > 0 ) { }
        app.quit();
      } );
    }

    // Start Qt event loop
    syslog( LOG_INFO, "Starting Qt event loop" );
    int result = app.exec();

    // Cleanup on exit
    remove_pid_file();
    cleanup_syslog();
    return result;
  }
  catch ( const std::exception& e )
  {
    syslog( LOG_ERR, "Failed to initialize daemon: %s", e.what() );
  }

  cleanup_syslog();
  return 1;
}

// Print usage information
void print_usage( std::string_view program_name )
{
  std::cout << "Usage: " << program_name << " [OPTIONS]\n"
            << "Options:\n"
            << "  --version      Show version information\n"
            << "  --help         Show this help message\n"
            << "  --debug        Run in debug mode (foreground with verbose logging)\n"
            << "  --start        Start the daemon\n"
            << "  --stop         Stop the running daemon\n";
}

// Print version information
void print_version()
{
  std::cout << DAEMON_NAME << " version " << VERSION << "\n"
            << "Daemon for Uniwill Control Center\n";
}

// Stop the running daemon via PID file
int stop_daemon()
{
  std::ifstream pidfile( PID_FILE.data() );
  if ( not pidfile.is_open() )
  {
    std::cerr << "PID file not found (" << PID_FILE << "). Daemon may not be running." << std::endl;
    return 1;
  }

  pid_t pid = 0;
  pidfile >> pid;
  pidfile.close();

  if ( pid <= 0 )
  {
    std::cerr << "Invalid PID in " << PID_FILE << std::endl;
    return 1;
  }

  // Check if the process is actually running
  if ( kill( pid, 0 ) != 0 )
  {
    std::cerr << "Daemon process (PID " << pid << ") is not running." << std::endl;
    // Best effort to remove stale PID file (ignore permission errors)
    try
    {
      std::filesystem::remove( PID_FILE );
    }
    catch ( const std::exception& e )
    {
      // Silently ignore if we can't remove (permission issue, will be cleaned up at next run)
    }
    return 1;
  }

  // Send SIGTERM
  if ( kill( pid, SIGTERM ) != 0 )
  {
    std::cerr << "Failed to send SIGTERM to PID " << pid << ": " << strerror( errno ) << std::endl;
    return 1;
  }

  std::cout << "SIGTERM sent to uccd daemon (PID " << pid << "). Waiting for exit..." << std::endl;

  // Wait up to 5 seconds for graceful shutdown
  for ( int i = 0; i < 50; ++i )
  {
    if ( kill( pid, 0 ) != 0 )
    {
      std::cout << "uccd daemon stopped." << std::endl;
      return 0;
    }
    usleep( 100000 ); // 100ms
  }

  // Force kill if still running
  std::cerr << "Daemon did not exit gracefully. Sending SIGKILL..." << std::endl;
  kill( pid, SIGKILL );
  usleep( 200000 );
  try
  {
    std::filesystem::remove( PID_FILE );
  }
  catch ( const std::exception& e )
  {
    // Ignore cleanup errors
  }
  std::cout << "uccd daemon force-killed." << std::endl;
  return 0;
}

int main( int argc, char* argv[] )
{
  std::vector<std::string> arguments;
  for ( int i = 1; i < argc; ++i )
  {
    arguments.push_back( argv[ i ] );
  }

  bool debug_mode = false;
  bool start_daemon = false;
  bool stop_daemon_flag = false;
  std::string new_settings_path;
  std::string new_profiles_path;

  // parse command-line arguments
  for ( size_t i = 0; i < arguments.size(); ++i )
  {
    const auto& arg = arguments[i];

    if ( arg == "--version" or arg == "-v" )
    {
      print_version();
      return 0;
    }
    else if ( arg == "--help" or arg == "-h" )
    {
      print_usage( argv[ 0 ] );
      return 0;
    }
    else if ( arg == "--debug" )
    {
      debug_mode = true;
    }
    else if ( arg == "--start" )
    {
      start_daemon = true;
    }
    else if ( arg == "--stop" )
    {
      stop_daemon_flag = true;
    }
  }

  // default action is to start
  if ( not debug_mode and not start_daemon and arguments.empty() )
  {
    start_daemon = true;
  }

  if ( debug_mode )
  {
    // Debug mode: run in foreground with logging to console
    std::cout << DAEMON_NAME << " version " << VERSION << " - Debug mode" << std::endl;
    std::cout << "Running in foreground with console logging..." << std::endl;
    if ( check_single_instance() != 0 )
    {
      return 1;
    }
    return run_daemon();
  }
  else if ( stop_daemon_flag )
  {
    // Stop mode: stop the running daemon
    return stop_daemon();
  }
  else if ( start_daemon )
  {
    // Normal mode: check for existing instance, daemonize and run
    if ( check_single_instance() != 0 )
    {
      return 1;
    }
    // Modern systemd-friendly behavior: do not daemonize here — run in foreground
    // so systemd (Type=simple) can supervise the process directly.
    return run_daemon();
  }

  return 0;
}
