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
#include <memory>
#include <optional>
#include <string>

/**
 * @brief Which clock domain to auto-overclock.
 */
enum class AutoOCComponent
{
  Core,
  Vram,
  Both, ///< Core first, then VRAM on top of stable core offset
};

/**
 * @brief Auto-OC algorithm mode.
 */
enum class AutoOCMode
{
  /// Raise offset as high as possible, detect instability via clock droop
  /// against a sliding reference.  GPU Boost dynamically changes the
  /// effective frequency cap, so the sliding window accounts for normal
  /// thermal/power drift.
  MaxOffset,
};

/**
 * @brief Result of a single stability test iteration.
 */
enum class StabilityResult
{
  Stable,
  Unstable,      ///< Clock droop or throttle detected
  ThermalLimit,  ///< Temperature exceeded threshold
  Aborted,       ///< User cancelled
};

/**
 * @brief Current phase of the auto-OC state machine.
 */
enum class AutoOCPhase
{
  Idle,
  Baseline,
  Searching,
  Validating,
  Done,
};

/**
 * @brief Tunable parameters for the auto-OC algorithm.
 */
struct AutoOCConfig
{
  AutoOCMode mode          = AutoOCMode::MaxOffset; ///< Algorithm mode
  int  resolutionMHz       = 5;     ///< Binary search step granularity
  int  safetyMarginCoreMHz = 15;    ///< Subtracted from max stable core offset
  int  safetyMarginVramMHz = 25;    ///< Subtracted from max stable VRAM offset
  int  searchTestMs        = 15000; ///< Per-step stability test duration (ms)
  int  validationTestMs    = 60000; ///< Final validation duration (ms)
  int  baselineMs          = 10000; ///< Baseline warmup duration (ms)
  int  settleMs            = 2000;  ///< Settling period after offset change (samples ignored)
  int  thermalLimitC       = 90;    ///< Abort temperature
  int  clockDroopMHz       = 50;    ///< Clock drop from *achieved* clock to declare unstable
  int  pollIntervalMs      = 500;   ///< How often to sample GPU metrics during a test
  int  minGpuUtilPct       = 70;    ///< Minimum GPU utilization to consider workload valid

  // ── VRAM ramp (linear walk with FPS-regression detection) ──
  // Instead of binary search, VRAM offset is ramped linearly.
  // At each step, FPS is measured.  When ECC error-correction overhead
  // causes throughput to decline (FPS drops), the ramp stops at the
  // peak-performing offset.  An optional deep-search pass refines the
  // peak with finer steps.
  int  vramRampStepMHz      = 25;   ///< VRAM offset increment per coarse ramp step
  int  vramRampTestMs       = 5000; ///< Post-settle test duration per ramp step (ms)
  int  vramFpsDropPct       = 2;    ///< FPS drop (%) from peak to count as regression
  int  vramConfirmSteps     = 2;    ///< Consecutive declining steps to confirm peak found
  bool vramDeepSearch       = true; ///< After coarse peak, refine with fine steps
  int  vramFineStepMHz      = 5;    ///< Fine step for deep-search refinement
};

/**
 * @brief Snapshot of auto-OC progress, emitted periodically.
 */
struct AutoOCProgress
{
  AutoOCPhase       phase         = AutoOCPhase::Idle;
  AutoOCComponent   component     = AutoOCComponent::Core;
  int               iteration     = 0;
  int               maxIterations = 0;
  int               currentOffsetMHz = 0;
  int               bestStableMHz = 0;
  StabilityResult   lastResult    = StabilityResult::Stable;
  int               tempC         = 0;
  int               gpuClockMHz   = 0;
  int               vramClockMHz  = 0;
  int               gpuUtilPct    = 0;
  double            fps           = -1.0;  ///< Live FPS from ucc-fps-layer, -1 = unavailable
  std::string       message;
};

/**
 * @brief Final result of the auto-OC procedure.
 */
struct AutoOCResult
{
  int  coreOffsetMHz = 0;
  int  vramOffsetMHz = 0;
  bool success       = false;
  std::string message;
};

/**
 * @brief Adaptive binary-search auto-overclock worker.
 *
 * ── MaxOffset mode ──
 *   Phase 0 — Thermal baseline: run for N seconds at offset=0, record baseline
 *             clock and temperature. Abort if already too hot.
 *   Phase 1 — Binary search: bisect [0, MAX_OFFSET] with stability tests.
 *             Monitors for clock droop via a sliding reference.
 *   Phase 2 — Validation: apply (maxStable − safetyMargin) and run extended
 *             stability test.
 *
 * ── VRAM ramp ──
 *   Instead of binary search, VRAM offset is ramped linearly while tracking
 *   FPS.  When FPS drops (ECC overhead), the ramp stops at the peak offset.
 *   An optional deep-search pass refines around the peak with finer steps.
 *
 * The worker expects the user (or an external tool) to supply GPU load while
 * the scan is running. If GPU utilization falls below the threshold the step
 * is retried rather than accepted.
 *
 * Threading: the worker uses a QTimer for its poll loop and must live on a
 * thread with a Qt event loop (e.g., the main thread). All public methods
 * are safe to call from the main thread.
 */
class AutoOCWorker : public QObject
{
  Q_OBJECT

public:
  using LogFn = std::function< void( const std::string & ) >;

  explicit AutoOCWorker( std::shared_ptr< NvmlWrapper > nvml,
                         LogFn logFn = {},
                         QObject *parent = nullptr );
  ~AutoOCWorker() override;

  AutoOCWorker( const AutoOCWorker & ) = delete;
  AutoOCWorker &operator=( const AutoOCWorker & ) = delete;

  /// @return true if NVML is initialized and offsets are writable.
  [[nodiscard]] bool isAvailable() const;

  /// Start the auto-OC scan. Returns false if already running or unavailable.
  bool start( AutoOCComponent component,
              unsigned int deviceIndex = 0,
              const AutoOCConfig &config = {} );

  /// Request cancellation (the current step finishes, then stops).
  void stop();

  /// @return true if a scan is in progress.
  [[nodiscard]] bool isRunning() const;

  /// @return Current config (read-only).
  [[nodiscard]] const AutoOCConfig &config() const { return m_config; }

  /// Bind this worker to the shared FPS server (owned by UccDBusInterfaceAdaptor).
  /// Must be called before start() if FPS measurement is desired.
  void setFpsServer( FpsServer *fps ) noexcept { m_fpsServer = fps; }

signals:
  /// Emitted every pollIntervalMs with current progress.
  void progress( const AutoOCProgress &prog );

  /// Emitted once when the scan completes or is aborted.
  void finished( const AutoOCResult &result );

private slots:
  void onPollTick();

private:
  // ── State machine ──
  void enterBaseline();
  void enterSearch();
  void enterValidation();
  void enterDone( bool success, const std::string &msg );

  // ── Stability testing ──
  StabilityResult sampleStability();
  StabilityResult sampleStabilityMaxOffset();
  StabilityResult sampleStabilityVramRamp();
  void applyOffset( int offsetMHz );
  void resetOffset();

  // ── Utilities ──
  int  maxOffsetForComponent() const;
  int  safetyMarginForComponent() const;
  int  ceilToResolution( int value ) const;
  int  floorToResolution( int value ) const;
  void log( const std::string &msg );
  void emitProgress( const std::string &msg = {} );

  // ── Crash-resume checkpointing ──
  void saveCheckpoint( bool force = false );
  void clearCheckpoint();
  bool tryResumeFromCheckpoint( AutoOCComponent requestedComponent,
                                unsigned int requestedDeviceIndex );

  // ── Members ──
  std::shared_ptr< NvmlWrapper > m_nvml;
  LogFn m_logFn;

  AutoOCConfig    m_config;
  AutoOCComponent m_component  = AutoOCComponent::Core;
  AutoOCComponent m_activeClk  = AutoOCComponent::Core; ///< Which clock we're currently scanning
  unsigned int    m_deviceIndex = 0;

  AutoOCPhase     m_phase      = AutoOCPhase::Idle;
  std::atomic_bool m_stopRequested{ false };

  // Binary search state
  int m_lo = 0;
  int m_hi = 0;
  int m_mid = 0;
  int m_iteration = 0;
  int m_maxIterations = 0;
  int m_bestStable = 0;

  // Baseline readings
  int m_baselineClockMHz = 0;
  int m_baselineTempC    = 0;

  // Baseline clock accumulation — gathers post-settle samples to compute
  // a robust average sustained clock.
  static constexpr int BASELINE_MAX_SAMPLES = 32;
  int  m_baselineClkSamples[BASELINE_MAX_SAMPLES] = {};
  int  m_baselineClkSampleCount = 0;

  // VRAM ramp state — linear walk with FPS-regression detection.
  // Tracks FPS at each offset step to find the peak before ECC
  // error-correction overhead degrades effective bandwidth.
  int    m_vramRampOffset      = 0;      ///< Current VRAM offset under test
  int    m_vramPeakOffset      = 0;      ///< Offset at which peak FPS was observed
  double m_vramPeakFps         = 0.0;    ///< Best avg FPS observed during ramp
  double m_vramBaselineFps     = 0.0;    ///< Avg FPS at offset=0 (from baseline)
  double m_rampFpsSum          = 0.0;    ///< Sum of FPS samples for current step
  int    m_rampSampleCount     = 0;      ///< Post-settle sample count for current ramp step
  int    m_rampDeclineCount    = 0;      ///< Consecutive declining steps (regression counter)
  bool   m_vramDeepPhase       = false;  ///< True when running fine-grained deep search
  int    m_vramCoarsePeakOffset = 0;     ///< Peak offset from coarse phase (for deep range)

  // Timing
  QTimer *m_pollTimer = nullptr;
  std::chrono::steady_clock::time_point m_stepStart;
  int m_stepDurationMs = 0;

  // Accumulated stability data for current step
  int  m_sampleCount     = 0;
  int  m_stableSampleCount = 0; ///< Samples after settling period
  int  m_droopCount      = 0;
  int  m_thermalCount    = 0;
  int  m_lowUtilCount    = 0;
  int  m_peakTempC       = 0;
  int  m_minClockMHz     = INT32_MAX;
  int  m_achievedClockMHz = 0;  ///< Peak clock observed during settling period

  // Sliding reference clock — tracks the GPU's natural frequency drift
  // (thermal/power throttle) so only sudden drops count as instability.
  static constexpr int    CLOCK_HISTORY_SIZE = 8;  ///< ~4 seconds at 500 ms poll
  int  m_clockHistory[CLOCK_HISTORY_SIZE] = {};
  int  m_clockHistoryIdx   = 0;
  int  m_clockHistoryCount = 0;
  int  m_slidingRefClockMHz = 0;  ///< Rolling minimum of recent samples

  // FPS measurement (requires ucc-fps-layer running in game process).
  // Non-owning pointer to the shared FpsServer in UccDBusInterfaceAdaptor;
  // nullptr means FPS measurement is unavailable.
  FpsServer *m_fpsServer = nullptr;

  // Results (when scanning "Both")
  int m_finalCoreOffset = 0;
  int m_finalVramOffset = 0;

  // Crash-resume checkpoint throttling
  std::chrono::steady_clock::time_point m_lastCheckpointPersist;
};
