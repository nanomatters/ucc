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

#include "FpsServer.hpp"
#include "NvmlWrapper.hpp"

#include <QObject>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Types
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Phase of the auto-undervolt state machine.
 */
enum class UVPhase
{
  Idle,       ///< Not running
  Baseline,   ///< Measuring baseline FPS / clocks at max settings
  Searching,  ///< Fixed-cap offset sweep (increase offset while stable)
  OffsetSearching, ///< Reserved for compatibility (unused)
  Validating, ///< Extended stability test of the chosen cap
  Done,       ///< Complete (success or failure)
};

enum class UVSearchAction
{
  None,
  LowerCap,
  RaiseOffset,
};

/**
 * @brief Configuration for the undervolt search.
 */
struct UndervoltConfig
{
  int  baselineMs       = 20000; ///< Warmup / baseline measurement duration (ms)
  int  searchTestMs     = 20000; ///< Per-step test duration during search (ms)
  int  validationMs     = 60000; ///< Final validation duration (ms)
  int  settleMs         = 2000;  ///< Settling period after clock change (ms)
  int  pollIntervalMs   = 500;   ///< GPU metric poll interval (ms)
  int  fpsCompareWindowMs = 5000; ///< Running FPS window used for pass/fail comparisons
  int  thermalLimitC    = 90;    ///< Abort temperature (°C)
  int  fpsDropPct       = 5;     ///< Max allowed FPS drop (%) before considering cap too low
  int  stepMHz          = 15;    ///< Frequency cap step size for binary search (MHz)
  int  offsetStepMHz    = 25;    ///< Core-offset search granularity (MHz)
  int  maxCoreOffsetMHz = 500;   ///< Upper clamp for core-offset tuning
  int  minGpuUtilPct    = 60;    ///< Minimum GPU util for valid workload
  int  minP0ResidencyPct = 75;   ///< Required percentage of samples in P0 during tests
  int  clockDroopMHz    = 60;    ///< Max sudden drop from sliding ref clock
  int  maxDroopPct      = 20;    ///< Allowed droop sample ratio per step
  int  maxPowerCapPct   = 85;    ///< Allowed "Power Limit" throttle ratio per step
  int  maxThermalThrottlePct = 10; ///< Allowed thermal/slowdown throttle ratio per step
  int  maxEventPowerPct = 85;    ///< Allowed power-related clocks-event ratio per step
  int  maxEventThermalPct = 15;  ///< Allowed thermal-related clocks-event ratio per step
  int  maxViolationPowerPct = 80; ///< Allowed ratio of samples with power violation increments
  int  maxViolationThermalPct = 20; ///< Allowed ratio of samples with thermal violation increments
  int  maxNvapiLimiterPct = 90;  ///< Allowed ratio of non-zero NvAPI perf limiter mask samples
  int  maxStepRetries   = 1;     ///< Retries per candidate to reduce noise false negatives
  int  safetyMarginMHz  = 15;    ///< Added above the lowest stable cap as safety margin
};

/**
 * @brief Progress snapshot emitted periodically.
 */
struct UndervoltProgress
{
  UVPhase     phase          = UVPhase::Idle;
  int         iteration      = 0;
  int         maxIterations  = 0;
  int         currentCapMHz  = 0;     ///< Current GPU frequency cap under test
  int         bestCapMHz     = 0;     ///< Best (lowest) stable cap found so far
  int         currentOffsetMHz = 0;   ///< Current core offset under test
  int         bestOffsetMHz    = 0;   ///< Best stable core offset at chosen cap
  int         tempC          = 0;
  int         gpuClockMHz    = 0;
  int         powerDrawW     = 0;
  int         gpuUtilPct     = 0;
  double      fps            = -1.0;
  double      baselineFps    = -1.0;
  std::string appName;
  std::string message;
};

/**
 * @brief Final result of the auto-undervolt procedure.
 */
struct UndervoltResult
{
  int         gpuFreqCapMHz  = 0;    ///< The applied GPU frequency cap
  int         coreOffsetMHz  = 0;    ///< Final core offset at P0
  int         baselineClkMHz = 0;    ///< Original baseline clock before undervolting
  double      baselineFps    = 0.0;
  double      finalFps       = 0.0;
  double      finalPowerW    = 0.0;
  double      baselineVoltageMv = 0.0;
  double      finalVoltageMv = 0.0;
  double      powerSavedPct  = 0.0;  ///< Estimated power saving vs baseline
  bool        success        = false;
  std::string appName;               ///< The app this profile was found for
  std::string message;
};

/**
 * @brief Per-app stored undervolt profile.
 *
 * Saved in the settings file so that when the same app is detected again
 * the frequency cap can be applied immediately without re-scanning.
 */
struct AppUndervoltProfile
{
  std::string appName;
  int         gpuFreqCapMHz  = 0;
  int         coreOffsetMHz  = 0;
  int         baselineClkMHz = 0;
  double      baselineFps    = 0.0;
  double      achievedFps    = 0.0;
  double      achievedPowerW = 0.0;
  double      achievedVoltageMv = 0.0;
  std::chrono::system_clock::time_point lastUsed;
};

// ─────────────────────────────────────────────────────────────────────────────
// Worker
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Per-application GPU frequency-cap undervolt worker.
 *
 * Algorithm (v3 fixed-cap offset sweep):
 *
 *   Phase 0  — **Baseline**: Non-invasive measurement of current sustained GPU
 *              clock and FPS under load for N seconds.
 *
 *   Phase 1  — **Offset sweep**: Lock cap to baseline clock and keep it fixed.
 *              Increase core offset by offsetStepMHz as long as each step stays
 *              stable. Stability gates use FPS, min-FPS floor, P0 residency,
 *              clock droop, throttle/event-reason ratios, violation-status
 *              deltas, and NvAPI perf-limiter activity.
 *              Failed candidates are retried once (configurable), then the
 *              search stops and keeps the last stable offset.
 *
 *   Phase 2  — **Validation**: Apply the last stable offset at fixed cap and run
 *              extended verification with the same stability gates.
 *
 *   The result is stored as a per-app profile keyed by the Vulkan layer's
 *   process name (from SO_PEERCRED).  When the same app reconnects, the stored
 *   cap is reapplied immediately.
 *
 * Why frequency cap and not power limit?
 *   Lowering the power limit causes GPU Boost to choose a lower bin in the
 *   V/F table internally, achieving undervolt.  But we keep power at MAX so
 *   the GPU has headroom; the frequency cap *forces* it to stay at a lower
 *   bin, achieving the same power saving with more deterministic control over
 *   the actual clock.
 *
 * Threading: QTimer-based, lives on the main thread.
 */
class AutoUndervoltWorker : public QObject
{
  Q_OBJECT

public:
  using LogFn = std::function< void( const std::string & ) >;

  explicit AutoUndervoltWorker( std::shared_ptr< NvmlWrapper > nvml,
                                LogFn logFn = {},
                                QObject *parent = nullptr );
  ~AutoUndervoltWorker() override;

  AutoUndervoltWorker( const AutoUndervoltWorker & ) = delete;
  AutoUndervoltWorker &operator=( const AutoUndervoltWorker & ) = delete;

  /// @return true if NVML is initialised and GPU locking is supported.
  [[nodiscard]] bool isAvailable() const;

  /// Start the undervolt scan for the app currently sending FPS data.
  /// Returns false if already running, no FPS client, or NVML unavailable.
  bool start( unsigned int deviceIndex = 0,
              const UndervoltConfig &config = {} );

  /// Request cancellation.
  void stop();

  /// @return true if a scan is in progress.
  [[nodiscard]] bool isRunning() const;

  /// Bind to the shared FPS server (must be called before start()).
  void setFpsServer( FpsServer *fps ) noexcept { m_fpsServer = fps; }

  /// Look up a stored per-app profile. Returns nullopt if none.
  [[nodiscard]] std::optional< AppUndervoltProfile >
  getAppProfile( const std::string &appName ) const;

  /// Apply a previously stored per-app profile (locks GPU clocks).
  bool applyAppProfile( const std::string &appName, unsigned int deviceIndex = 0 );

  /// Remove a stored per-app profile.
  void removeAppProfile( const std::string &appName );

  /// Clear all stored per-app profiles.
  void clearAllProfiles();

  /// @return all stored per-app profiles (for serialisation).
  [[nodiscard]] const std::map< std::string, AppUndervoltProfile > &profiles() const
  { return m_appProfiles; }

  /// Restore profiles from serialised data (called on daemon start).
  void loadProfiles( const std::map< std::string, AppUndervoltProfile > &profiles );

signals:
  /// Emitted every pollIntervalMs with current progress.
  void progress( const UndervoltProgress &prog );

  /// Emitted once when the scan completes or is aborted.
  void finished( const UndervoltResult &result );

private slots:
  void onPollTick();

private:
  // ── State machine ──
  void enterBaseline();
  void enterSearch();
  void enterOffsetSearch();
  void enterValidation();
  void enterDone( bool success, const std::string &msg );
  void resetStepMetrics();
  bool evaluateStep( double &avgFpsOut,
                     bool &validWorkloadOut,
                     int &p0ResidencyPctOut,
                     int &droopPctOut,
                     int &powerCapPctOut,
                     int &thermalThrottlePctOut,
                     int &eventPowerPctOut,
                     int &eventThermalPctOut,
                     int &violationPowerPctOut,
                     int &violationThermalPctOut,
                     int &nvapiLimiterPctOut,
                     double &minFpsOut ) const;
  bool startNextSearchCandidate( UVSearchAction preferredAction );
  bool applyCandidate( int capMHz, int offsetMHz, UVSearchAction action );

  // ── Helpers ──
  void applyFreqCap( int capMHz );
  void resetFreqCap();
  void applyCoreOffset( int offsetMHz );
  void captureOriginalState();
  void restoreOriginalState( bool keepCurrentCap );
  void ensureMaxPowerLimit();
  void log( const std::string &msg );
  void emitProgress( const std::string &msg = {} );

  // ── Members ──
  std::shared_ptr< NvmlWrapper > m_nvml;
  LogFn m_logFn;

  UndervoltConfig m_config;
  unsigned int    m_deviceIndex = 0;
  UVPhase         m_phase       = UVPhase::Idle;
  std::atomic_bool m_stopRequested{ false };

  // FPS server (non-owning)
  FpsServer *m_fpsServer = nullptr;

  // Current app identification
  std::string m_appName;

  // Baseline measurements
  int    m_baselineClockMHz = 0;
  double m_baselineFps      = 0.0;
  double m_targetFps         = 0.0;  ///< Baseline reference FPS (fixed after baseline)
  double m_fpsThreshold      = 0.0;  ///< Fixed pass/fail threshold derived from baseline
  double m_peakFpsObserved   = 0.0;  ///< Running peak FPS across all samples
  double m_baselinePowerW   = 0.0;
  double m_baselineVoltageMv = 0.0;
  int    m_baselineTempC    = 0;

  // FPS accumulation for current step
  double m_fpsAccum     = 0.0;
  int    m_fpsSamples   = 0;
  std::vector< double > m_baselineFpsSamples;
  std::vector< double > m_stepFpsSamples;

  // Power accumulation for current step
  double m_powerAccum   = 0.0;
  int    m_powerSamples = 0;

  // Voltage accumulation for current step
  double m_voltageAccum = 0.0;
  int    m_voltageSamples = 0;

  // FPS floor for validation metrics
  double m_minFpsObserved = -1.0;

  // Clock accumulation for baseline
  static constexpr int MAX_CLK_SAMPLES = 32;
  int m_clkSamples[ MAX_CLK_SAMPLES ] = {};
  int m_clkSampleCount = 0;

  // Binary search state
  int m_lo            = 0;  ///< Lowest cap to consider (MHz)
  int m_hi            = 0;  ///< Highest cap (= baseline clock)
  int m_mid           = 0;  ///< Current test cap
  int m_bestCap       = 0;  ///< Lowest stable cap found so far
  int m_iteration     = 0;
  int m_maxIterations = 0;

  // Offset search state (at chosen cap)
  int m_offsetLo = 0;
  int m_offsetHi = 0;
  int m_offsetMid = 0;
  int m_bestOffset = 0;
  int m_finalCapMHz = 0;

  // Fixed-cap offset sweep state (v3)
  int m_searchCapMHz = 0;
  int m_searchOffsetMHz = 0;
  int m_stableCapMHz = 0;
  int m_stableOffsetMHz = 0;
  UVSearchAction m_lastAction = UVSearchAction::None;
  UVSearchAction m_nextAction = UVSearchAction::RaiseOffset;
  bool m_blockedLowerCap = false;
  bool m_blockedRaiseOffset = false;
  int m_stepRetryCount = 0;

  // Timing
  QTimer *m_pollTimer = nullptr;
  std::chrono::steady_clock::time_point m_stepStart;
  int m_stepDurationMs = 0;

  // Stability data
  int m_sampleCount   = 0;
  int m_postSettleSamples = 0;
  int m_lowUtilCount  = 0;
  int m_peakTempC     = 0;
  int m_pstateSamples = 0;
  int m_p0Samples     = 0;
  int m_droopCount    = 0;
  int m_powerCapCount = 0;
  int m_thermalThrottleCount = 0;
  int m_eventPowerCount = 0;
  int m_eventThermalCount = 0;
  int m_violationPowerDeltaCount = 0;
  int m_violationThermalDeltaCount = 0;
  int m_nvapiLimiterCount = 0;
  std::optional< unsigned long long > m_prevPowerViolationUsec;
  std::optional< unsigned long long > m_prevThermalViolationUsec;
  static constexpr int CLOCK_HISTORY_SIZE = 8;
  int m_clockHistory[CLOCK_HISTORY_SIZE] = {};
  int m_clockHistoryIdx = 0;
  int m_clockHistoryCount = 0;
  int m_slidingRefClockMHz = 0;

  // Per-app stored profiles
  std::map< std::string, AppUndervoltProfile > m_appProfiles;

  // Original GPU state snapshot for safe restoration
  std::optional< double > m_originalPowerLimitW;
  std::optional< std::pair< unsigned int, unsigned int > > m_originalGpuLockedClocks;
  std::optional< int > m_originalCoreOffsetMHz;

  // Max power limit (read once at start)
  double m_maxPowerW = 0.0;
};
