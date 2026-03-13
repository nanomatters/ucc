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

#include "workers/AutoOCWorker.hpp"

#include "FpsServer.hpp"
#include "OverlayShmWriter.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <syslog.h>

// ─────────────────────────────────────────────────────────────────────────────
// Construction / destruction
// ─────────────────────────────────────────────────────────────────────────────

AutoOCWorker::AutoOCWorker( std::shared_ptr< NvmlWrapper > nvml,
                            LogFn logFn,
                            QObject *parent )
  : QObject( parent )
  , m_nvml( std::move( nvml ) )
  , m_logFn( std::move( logFn ) )
{
  m_pollTimer = new QTimer( this );
  m_pollTimer->setTimerType( Qt::PreciseTimer );
  connect( m_pollTimer, &QTimer::timeout, this, &AutoOCWorker::onPollTick );
}

AutoOCWorker::~AutoOCWorker()
{
  if ( isRunning() )
  {
    m_stopRequested = true;
    m_pollTimer->stop();
    resetOffset();
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

bool AutoOCWorker::isAvailable() const
{
  return m_nvml && m_nvml->isAvailable() && m_nvml->deviceCount() > 0;
}

bool AutoOCWorker::start( AutoOCComponent component,
                          unsigned int deviceIndex,
                          const AutoOCConfig &config )
{
  if ( !isAvailable() )
  {
    log( "AutoOC: NVML not available" );
    return false;
  }

  if ( isRunning() )
  {
    log( "AutoOC: already running" );
    return false;
  }

  // Verify offsets are writable on P0 for the requested component(s)
  if ( component == AutoOCComponent::Core || component == AutoOCComponent::Both )
  {
    if ( !m_nvml->isClockOffsetWritable( deviceIndex, nvml::NVML_CLOCK_GRAPHICS, nvml::NVML_PSTATE_0 ) )
    {
      log( "AutoOC: GPU core offsets not writable on P0" );
      return false;
    }
  }
  if ( component == AutoOCComponent::Vram || component == AutoOCComponent::Both )
  {
    if ( !m_nvml->isClockOffsetWritable( deviceIndex, nvml::NVML_CLOCK_MEM, nvml::NVML_PSTATE_0 ) )
    {
      log( "AutoOC: VRAM offsets not writable on P0" );
      return false;
    }
  }

  m_config       = config;
  m_component    = component;
  m_deviceIndex  = deviceIndex;
  m_stopRequested = false;
  m_finalCoreOffset = 0;
  m_finalVramOffset = 0;

  // Determine which clock to start with
  m_activeClk = ( component == AutoOCComponent::Vram ) ? AutoOCComponent::Vram
                                                       : AutoOCComponent::Core;

  log( "AutoOC: starting scan — component="
       + std::string( component == AutoOCComponent::Core ? "core"
                      : component == AutoOCComponent::Vram ? "vram"
                      : "both" )
       + " device=" + std::to_string( deviceIndex ) );

  if ( m_fpsServer && m_fpsServer->start() )
    log( "AutoOC: FPS socket server started" );
  else if ( m_fpsServer )
    log( "AutoOC: FPS socket server unavailable (no layer data)" );

  enterBaseline();
  return true;
}

void AutoOCWorker::stop()
{
  if ( !isRunning() )
    return;
  m_stopRequested = true;
  log( "AutoOC: stop requested" );
}

bool AutoOCWorker::isRunning() const
{
  return m_phase != AutoOCPhase::Idle && m_phase != AutoOCPhase::Done;
}

// ─────────────────────────────────────────────────────────────────────────────
// State machine transitions
// ─────────────────────────────────────────────────────────────────────────────

void AutoOCWorker::enterBaseline()
{
  m_phase = AutoOCPhase::Baseline;

  // Reset offset to 0 for baseline measurement
  applyOffset( 0 );

  m_stepStart      = std::chrono::steady_clock::now();
  m_stepDurationMs = m_config.baselineMs;
  m_sampleCount    = 0;
  m_stableSampleCount = 0;
  m_droopCount     = 0;
  m_thermalCount   = 0;
  m_lowUtilCount   = 0;
  m_peakTempC      = 0;
  m_minClockMHz    = INT32_MAX;
  m_achievedClockMHz = 0;
  m_clockHistoryIdx   = 0;
  m_clockHistoryCount = 0;
  m_slidingRefClockMHz = 0;

  m_baselineClockMHz = 0;
  m_baselineTempC    = 0;
  m_baselineClkSampleCount = 0;

  // Reset VRAM ramp state
  m_vramRampOffset      = 0;
  m_vramPeakOffset      = 0;
  m_vramPeakFps         = 0.0;
  m_vramBaselineFps     = 0.0;
  m_rampFpsSum          = 0.0;
  m_rampSampleCount     = 0;
  m_rampDeclineCount    = 0;
  m_vramDeepPhase       = false;
  m_vramCoarsePeakOffset = 0;

  log( "AutoOC: Phase 0 — Baseline (" + std::to_string( m_config.baselineMs / 1000 ) + " s)"
       + ( m_activeClk == AutoOCComponent::Vram ? " [VRAM ramp mode]"
           : " [max-offset mode]" ) );
  emitProgress( "Measuring baseline..." );

  m_pollTimer->start( m_config.pollIntervalMs );
}

void AutoOCWorker::enterSearch()
{
  m_phase = AutoOCPhase::Searching;
  m_bestStable = 0;
  m_iteration = 0;

  // Snapshot FPS at the end of the baseline phase so we can report improvement.
  if ( m_fpsServer ) m_fpsServer->snapshotBaseline();
  if ( m_fpsServer && m_fpsServer->baselineFps() > 0.0 )
    log( "AutoOC: baseline FPS = " + std::to_string( static_cast<int>( m_fpsServer->baselineFps() ) ) );

  if ( m_activeClk == AutoOCComponent::Vram )
  {
    // ── VRAM ramp: linear walk with FPS-regression detection ──
    // Compute baseline FPS from samples accumulated during baseline.
    if ( m_rampSampleCount >= 4 )
    {
      m_vramBaselineFps = m_rampFpsSum / static_cast< double >( m_rampSampleCount );
    }
    else if ( m_fpsServer )
    {
      double fps = m_fpsServer->currentFps();
      m_vramBaselineFps = ( fps > 0.0 ) ? fps : 0.0;
    }
    else
    {
      m_vramBaselineFps = 0.0;
    }

    if ( m_vramBaselineFps <= 0.0 )
    {
      enterDone( false, "VRAM: no FPS data available — ensure ucc-fps-layer is running with UCC_FPS_HOOK=1" );
      return;
    }

    m_vramPeakFps    = m_vramBaselineFps;
    m_vramPeakOffset = 0;
    m_rampDeclineCount = 0;
    m_vramDeepPhase  = false;
    m_vramCoarsePeakOffset = 0;
    m_vramRampOffset = m_config.vramRampStepMHz; // first step

    m_hi = maxOffsetForComponent();
    m_maxIterations = m_hi / m_config.vramRampStepMHz;
    m_mid = m_vramRampOffset;

    log( "AutoOC: VRAM baseline — FPS=" + std::to_string( static_cast< int >( m_vramBaselineFps ) ) );
    log( "AutoOC: Phase 1 — VRAM linear ramp [+" + std::to_string( m_config.vramRampStepMHz )
         + ", " + std::to_string( m_hi )
         + "] MHz, step=" + std::to_string( m_config.vramRampStepMHz )
         + " MHz, ~" + std::to_string( m_maxIterations ) + " steps" );

    applyOffset( m_vramRampOffset );

    m_stepStart      = std::chrono::steady_clock::now();
    m_stepDurationMs = m_config.settleMs + m_config.vramRampTestMs;

    // Reset per-step counters
    m_rampFpsSum     = 0.0;
    m_rampSampleCount    = 0;
    m_sampleCount        = 0;
    m_stableSampleCount  = 0;
    m_droopCount     = 0;
    m_thermalCount   = 0;
    m_lowUtilCount   = 0;
    m_peakTempC      = 0;
    m_minClockMHz    = INT32_MAX;
    m_achievedClockMHz = 0;
    m_clockHistoryIdx   = 0;
    m_clockHistoryCount = 0;
    m_slidingRefClockMHz = 0;

    emitProgress( "VRAM ramp: testing +" + std::to_string( m_vramRampOffset ) + " MHz" );
  }
  else
  {
    // ── Core: binary search ──
    m_lo = 0;
    m_hi = maxOffsetForComponent();
    m_maxIterations = static_cast< int >(
      std::ceil( std::log2( static_cast< double >( m_hi ) / m_config.resolutionMHz ) ) ) + 1;

    m_mid = ceilToResolution( m_hi / 3 );

    log( "AutoOC: Phase 1 — Binary search [0, " + std::to_string( m_hi )
         + "] MHz, start=" + std::to_string( m_mid )
         + " MHz, ~" + std::to_string( m_maxIterations ) + " iterations" );

    applyOffset( m_mid );

    m_stepStart      = std::chrono::steady_clock::now();
    m_stepDurationMs = m_config.searchTestMs;
    m_sampleCount    = 0;
    m_stableSampleCount = 0;
    m_droopCount     = 0;
    m_thermalCount   = 0;
    m_lowUtilCount   = 0;
    m_peakTempC      = 0;
    m_minClockMHz    = INT32_MAX;
    m_achievedClockMHz = 0;
    m_clockHistoryIdx   = 0;
    m_clockHistoryCount = 0;
    m_slidingRefClockMHz = 0;

    emitProgress( "Testing offset +" + std::to_string( m_mid ) + " MHz" );
  }
}

void AutoOCWorker::enterValidation()
{
  m_phase = AutoOCPhase::Validating;

  int margin = safetyMarginForComponent();
  int candidate = std::max( 0, m_bestStable - margin );
  candidate = floorToResolution( candidate );

  log( "AutoOC: Phase 2 — Validation at +" + std::to_string( candidate )
       + " MHz (best=" + std::to_string( m_bestStable )
       + ", margin=" + std::to_string( margin ) + ")" );

  applyOffset( candidate );

  m_mid            = candidate; // reuse m_mid for the validation offset
  m_stepStart      = std::chrono::steady_clock::now();
  m_stepDurationMs = m_config.validationTestMs;
  m_sampleCount    = 0;
  m_stableSampleCount = 0;
  m_droopCount     = 0;
  m_thermalCount   = 0;
  m_lowUtilCount   = 0;
  m_peakTempC      = 0;
  m_minClockMHz    = INT32_MAX;
  m_achievedClockMHz = 0;
  m_clockHistoryIdx   = 0;
  m_clockHistoryCount = 0;
  m_slidingRefClockMHz = 0;

  // Reset VRAM ramp per-step counters (used during VRAM validation)
  m_rampFpsSum      = 0.0;
  m_rampSampleCount = 0;

  emitProgress( "Validating +" + std::to_string( candidate ) + " MHz (" +
                std::to_string( m_config.validationTestMs / 1000 ) + " s)" );
}

void AutoOCWorker::enterDone( bool success, const std::string &msg )
{
  m_pollTimer->stop();
  m_phase = AutoOCPhase::Done;

  if ( !success )
  {
    resetOffset();
  }

  // Report FPS improvement if we have both baseline and final data.
  if ( m_fpsServer && m_fpsServer->baselineFps() > 0.0 && m_fpsServer->currentFps() > 0.0 )
  {
    double imp = m_fpsServer->improvementPct();
    log( "AutoOC: FPS baseline=" + std::to_string( static_cast<int>( m_fpsServer->baselineFps() ) )
         + " final=" + std::to_string( static_cast<int>( m_fpsServer->currentFps() ) )
         + ( imp >= 0.0 ? " (+" : " (" ) + std::to_string( static_cast<int>( imp ) ) + "%)" );
  }
  if ( m_fpsServer ) m_fpsServer->stop();

  log( "AutoOC: " + msg );

  AutoOCResult result;
  result.coreOffsetMHz = m_finalCoreOffset;
  result.vramOffsetMHz = m_finalVramOffset;
  result.success = success;
  result.message = msg;

  emitProgress( msg );
  OverlayShmWriter::instance().setInactive();
  emit finished( result );
}

// ─────────────────────────────────────────────────────────────────────────────
// Poll tick — the heart of the state machine
// ─────────────────────────────────────────────────────────────────────────────

void AutoOCWorker::onPollTick()
{
  // Drain any pending FPS data from the layer socket.
  if ( m_fpsServer ) m_fpsServer->poll();

  if ( m_stopRequested )
  {
    enterDone( false, "Scan cancelled by user" );
    return;
  }

  auto elapsed = std::chrono::steady_clock::now() - m_stepStart;
  int elapsedMs = static_cast< int >(
    std::chrono::duration_cast< std::chrono::milliseconds >( elapsed ).count() );
  bool stepComplete = ( elapsedMs >= m_stepDurationMs );

  // ── Sample current GPU state ──
  StabilityResult sample = sampleStability();

  // ── Phase dispatch ──
  switch ( m_phase )
  {
  // ────────────────────────────────────────────────────────────────────────
  case AutoOCPhase::Baseline:
  {
    if ( sample == StabilityResult::ThermalLimit )
    {
      enterDone( false, "GPU already at thermal limit (" +
                 std::to_string( m_peakTempC ) + " °C) — cannot scan" );
      return;
    }

    if ( stepComplete )
    {
      // Compute baseline from accumulated samples for a robust average.
      // Falls back to a single read if no samples were accumulated.
      auto temp = m_nvml->getTemperatureDegC( m_deviceIndex );
      m_baselineTempC = static_cast< int >( temp.value_or( 0 ) );

      if ( m_baselineClkSampleCount >= 4 )
      {
        // Sort samples and take the 25th-percentile (conservative sustained clock)
        std::sort( m_baselineClkSamples,
                   m_baselineClkSamples + m_baselineClkSampleCount );
        int p25idx = m_baselineClkSampleCount / 4;
        m_baselineClockMHz = m_baselineClkSamples[p25idx];
      }
      else
      {
        auto clk = m_nvml->getGpuClockMHz( m_deviceIndex );
        m_baselineClockMHz = static_cast< int >( clk.value_or( 0 ) );
      }

      if ( m_baselineClockMHz == 0 )
      {
        enterDone( false, "Could not read baseline GPU clock" );
        return;
      }

      log( "AutoOC: Baseline — clock=" + std::to_string( m_baselineClockMHz )
           + " MHz, temp=" + std::to_string( m_baselineTempC ) + " °C" );

      enterSearch();
    }
    else
    {
      emitProgress( "Baseline: " + std::to_string( elapsedMs / 1000 ) + "/"
                    + std::to_string( m_stepDurationMs / 1000 ) + " s" );
    }
    break;
  }

  // ────────────────────────────────────────────────────────────────────────
  case AutoOCPhase::Searching:
  {
    if ( m_activeClk == AutoOCComponent::Vram )
    {
      // ═══════════════════════════════════════════════════════════════════
      // VRAM linear ramp with FPS-regression detection
      // ═══════════════════════════════════════════════════════════════════

      // Thermal hard-stop
      if ( sample == StabilityResult::ThermalLimit )
      {
        log( "AutoOC: VRAM ramp — thermal limit at +" + std::to_string( m_vramRampOffset ) + " MHz" );
        m_bestStable = m_vramPeakOffset;
        if ( m_bestStable <= 0 )
        {
          enterDone( false, "VRAM: thermal limit before any beneficial offset" );
          return;
        }
        enterValidation();
        return;
      }

      if ( !stepComplete )
      {
        // Wait for step to complete — no early termination for VRAM ramp
        emitProgress( "VRAM +" + std::to_string( m_vramRampOffset ) + " MHz ("
                      + std::to_string( elapsedMs / 1000 ) + "/"
                      + std::to_string( m_stepDurationMs / 1000 ) + " s)"
                      + ( m_vramDeepPhase ? " [deep]" : "" ) );
        break;
      }

      // ── Step complete — evaluate FPS ──
      double avgFps = ( m_rampSampleCount > 0 )
        ? m_rampFpsSum / static_cast< double >( m_rampSampleCount ) : 0.0;

      int currentStep = m_vramDeepPhase ? m_config.vramFineStepMHz : m_config.vramRampStepMHz;

      log( "AutoOC: VRAM +" + std::to_string( m_vramRampOffset )
           + " MHz — FPS=" + std::to_string( static_cast< int >( avgFps ) )
           + " (peak=" + std::to_string( static_cast< int >( m_vramPeakFps ) )
           + " @+" + std::to_string( m_vramPeakOffset ) + ")"
           + ( m_vramDeepPhase ? " [deep]" : "" ) );

      // ── Peak tracking and regression detection ──
      bool declined = false;
      double dropThresh = m_vramPeakFps * static_cast< double >( m_config.vramFpsDropPct ) / 100.0;

      if ( avgFps > m_vramPeakFps + 0.5 )
      {
        // New FPS peak
        m_vramPeakFps    = avgFps;
        m_vramPeakOffset = m_vramRampOffset;
        m_rampDeclineCount = 0;
        log( "AutoOC: VRAM new peak at +" + std::to_string( m_vramPeakOffset )
             + " MHz (" + std::to_string( static_cast< int >( m_vramPeakFps ) ) + " FPS)" );
      }
      else if ( avgFps < m_vramPeakFps - dropThresh )
      {
        // Performance dropped — possible ECC overhead
        ++m_rampDeclineCount;
        declined = true;
        log( "AutoOC: VRAM FPS regression step " + std::to_string( m_rampDeclineCount )
             + "/" + std::to_string( m_config.vramConfirmSteps )
             + " at +" + std::to_string( m_vramRampOffset ) + " MHz" );
      }
      else
      {
        // Plateau — within tolerance of peak
        if ( avgFps >= m_vramPeakFps - 0.5 )
          m_vramPeakOffset = m_vramRampOffset;
        m_rampDeclineCount = 0;
      }
      (void) declined;

      // ── Regression confirmed? ──
      if ( m_rampDeclineCount >= m_config.vramConfirmSteps )
      {
        if ( !m_vramDeepPhase && m_config.vramDeepSearch && m_vramPeakOffset > 0 )
        {
          // ── Enter deep search: fine-step ramp around coarse peak ──
          m_vramDeepPhase = true;
          m_vramCoarsePeakOffset = m_vramPeakOffset;
          double coarsePeakFps   = m_vramPeakFps;

          // Start fine search from (coarsePeak - coarseStep) to explore
          // the gap the coarse ramp may have skipped over.
          int fineStart = std::max( m_config.vramFineStepMHz,
                                    m_vramCoarsePeakOffset - m_config.vramRampStepMHz + m_config.vramFineStepMHz );

          m_vramRampOffset   = fineStart;
          m_vramPeakFps      = coarsePeakFps;
          m_vramPeakOffset   = m_vramCoarsePeakOffset; // retain coarse peak as initial best
          m_rampDeclineCount = 0;
          m_iteration        = 0;
          m_maxIterations    = ( m_vramCoarsePeakOffset + m_config.vramRampStepMHz - fineStart )
                               / m_config.vramFineStepMHz + 1;

          log( "AutoOC: VRAM entering deep search around +" + std::to_string( m_vramCoarsePeakOffset )
               + " MHz, range [" + std::to_string( fineStart ) + ", "
               + std::to_string( m_vramCoarsePeakOffset + m_config.vramRampStepMHz ) + "] MHz, step="
               + std::to_string( m_config.vramFineStepMHz ) + " MHz" );

          m_mid = m_vramRampOffset;
          applyOffset( m_vramRampOffset );

          m_stepStart      = std::chrono::steady_clock::now();
          m_stepDurationMs = m_config.settleMs + m_config.vramRampTestMs;
          m_rampFpsSum     = 0.0;
          m_rampSampleCount    = 0;
          m_sampleCount        = 0;
          m_stableSampleCount  = 0;
          m_droopCount     = 0;
          m_thermalCount   = 0;
          m_lowUtilCount   = 0;
          m_peakTempC      = 0;
          m_minClockMHz    = INT32_MAX;
          m_achievedClockMHz = 0;

          emitProgress( "VRAM deep: testing +" + std::to_string( m_vramRampOffset ) + " MHz" );
          break;
        }

        // Regression confirmed (or deep search done) — use peak
        m_bestStable = m_vramPeakOffset;
        log( "AutoOC: VRAM" + std::string( m_vramDeepPhase ? " deep" : "" )
             + " regression confirmed — peak at +"
             + std::to_string( m_vramPeakOffset ) + " MHz ("
             + std::to_string( static_cast< int >( m_vramPeakFps ) ) + " FPS)" );

        if ( m_bestStable <= 0 )
        {
          enterDone( false, "VRAM: no beneficial offset found (regression at first steps)" );
          return;
        }
        enterValidation();
        return;
      }

      // ── Advance ramp ──
      ++m_iteration;
      m_vramRampOffset += currentStep;

      // Check bounds: for deep search, stop at coarsePeak + coarseStep
      int rampCeiling = m_vramDeepPhase
        ? ( m_vramCoarsePeakOffset + m_config.vramRampStepMHz )
        : maxOffsetForComponent();

      if ( m_vramRampOffset > rampCeiling )
      {
        // Hit limit — use peak
        m_bestStable = m_vramPeakOffset;
        log( "AutoOC: VRAM reached " + std::string( m_vramDeepPhase ? "deep search" : "max offset" )
             + " limit — peak at +" + std::to_string( m_vramPeakOffset ) + " MHz" );

        if ( m_bestStable <= 0 )
        {
          enterDone( false, "VRAM: no beneficial offset found" );
          return;
        }
        enterValidation();
        return;
      }

      // Apply next offset and reset per-step counters
      m_mid = m_vramRampOffset;
      applyOffset( m_vramRampOffset );

      m_stepStart      = std::chrono::steady_clock::now();
      m_stepDurationMs = m_config.settleMs + m_config.vramRampTestMs;
      m_rampFpsSum     = 0.0;
      m_rampSampleCount    = 0;
      m_sampleCount        = 0;
      m_stableSampleCount  = 0;
      m_droopCount     = 0;
      m_thermalCount   = 0;
      m_lowUtilCount   = 0;
      m_peakTempC      = 0;
      m_minClockMHz    = INT32_MAX;
      m_achievedClockMHz = 0;

      emitProgress( "VRAM +" + std::to_string( m_vramRampOffset ) + " MHz (step "
                    + std::to_string( m_iteration + 1 ) + "/" + std::to_string( m_maxIterations ) + ")" );
      break;
    }
    else
    {
      // ═══════════════════════════════════════════════════════════════════
      // Core binary search (unchanged)
      // ═══════════════════════════════════════════════════════════════════

      // Early termination on thermal limit
      if ( sample == StabilityResult::ThermalLimit )
      {
        log( "AutoOC: Thermal limit at +" + std::to_string( m_mid ) + " MHz — reducing hi" );
        m_hi = m_mid;
      }
      else if ( !stepComplete )
      {
        // Check for early instability detection (>30% of post-settle samples show droop)
        if ( m_stableSampleCount >= 6 && m_droopCount * 100 / m_stableSampleCount > 30 )
        {
          log( "AutoOC: Early instability at +" + std::to_string( m_mid )
               + " MHz (" + std::to_string( m_droopCount ) + "/" + std::to_string( m_stableSampleCount )
               + " droop, achieved=" + std::to_string( m_achievedClockMHz ) + " MHz)" );
          m_hi = m_mid;
          // Fall through to advance step
        }
        else
        {
          emitProgress( "Testing +" + std::to_string( m_mid ) + " MHz ("
                        + std::to_string( elapsedMs / 1000 ) + "/"
                        + std::to_string( m_stepDurationMs / 1000 ) + " s)" );
          break; // wait for more samples
        }
      }
      else
      {
        // Stable if:  no thermal issue AND droop in less than 20% of post-settle samples
        //             AND had sufficient GPU load (low-util samples < 30%)
        bool hadLoad = ( m_stableSampleCount > 0 ) &&
                       ( m_lowUtilCount * 100 / m_stableSampleCount < 30 );

        if ( !hadLoad && m_stableSampleCount >= 4 )
        {
          // Insufficient GPU load — warn but treat as stable (can't tell)
          log( "AutoOC: Low GPU utilization at +" + std::to_string( m_mid )
               + " MHz — result may be unreliable" );
        }

        bool stable = ( m_droopCount * 100 / std::max( m_stableSampleCount, 1 ) < 20 );

        if ( stable )
        {
          m_bestStable = m_mid;
          m_lo = m_mid;
          log( "AutoOC: +" + std::to_string( m_mid ) + " MHz STABLE (achieved="
               + std::to_string( m_achievedClockMHz ) + " MHz)" );
        }
        else
        {
          m_hi = m_mid;
          log( "AutoOC: +" + std::to_string( m_mid ) + " MHz UNSTABLE ("
               + std::to_string( m_droopCount ) + "/" + std::to_string( m_stableSampleCount )
               + " droop, achieved=" + std::to_string( m_achievedClockMHz ) + " MHz)" );
        }
      }

      // ── Advance binary search ──
      ++m_iteration;

      if ( m_hi - m_lo <= m_config.resolutionMHz || m_iteration >= m_maxIterations + 2 )
      {
        // Search complete
        log( "AutoOC: Search complete — best stable offset: +"
             + std::to_string( m_bestStable ) + " MHz" );

        if ( m_bestStable <= 0 )
        {
          enterDone( false, "No stable offset found above 0 MHz" );
          return;
        }

        enterValidation();
        return;
      }

      // Next midpoint
      m_mid = ceilToResolution( ( m_lo + m_hi ) / 2 );

      // Clamp mid to avoid re-testing the same value
      if ( m_mid <= m_lo ) m_mid = m_lo + m_config.resolutionMHz;
      if ( m_mid >= m_hi ) m_mid = m_hi - m_config.resolutionMHz;
      if ( m_mid <= 0 ) m_mid = m_config.resolutionMHz;

      applyOffset( m_mid );

      m_stepStart      = std::chrono::steady_clock::now();
      m_stepDurationMs = m_config.searchTestMs;
      m_sampleCount    = 0;
      m_stableSampleCount = 0;
      m_droopCount     = 0;
      m_thermalCount   = 0;
      m_lowUtilCount   = 0;
      m_peakTempC      = 0;
      m_minClockMHz    = INT32_MAX;
      m_achievedClockMHz = 0;
      m_clockHistoryIdx   = 0;
      m_clockHistoryCount = 0;
      m_slidingRefClockMHz = 0;

      emitProgress( "Testing +" + std::to_string( m_mid ) + " MHz (step "
                    + std::to_string( m_iteration + 1 ) + "/" + std::to_string( m_maxIterations ) + ")" );
      break;
    }
  }

  // ────────────────────────────────────────────────────────────────────────
  case AutoOCPhase::Validating:
  {
    if ( sample == StabilityResult::ThermalLimit )
    {
      // Reduce by extra margin and try once more
      int doubleMargin = safetyMarginForComponent() * 2;
      int reduced = std::max( 0, m_bestStable - doubleMargin );
      reduced = floorToResolution( reduced );

      if ( reduced <= 0 )
      {
        enterDone( false, "Thermal limit during validation — no safe offset" );
        return;
      }

      log( "AutoOC: Thermal during validation — retrying with double margin at +"
           + std::to_string( reduced ) + " MHz" );

      applyOffset( reduced );
      m_mid            = reduced;
      m_stepStart      = std::chrono::steady_clock::now();
      m_sampleCount    = 0;
      m_stableSampleCount = 0;
      m_droopCount     = 0;
      m_thermalCount   = 0;
      m_lowUtilCount   = 0;
      m_peakTempC      = 0;
      m_minClockMHz    = INT32_MAX;
      m_achievedClockMHz = 0;
      m_clockHistoryIdx   = 0;
      m_clockHistoryCount = 0;
      m_slidingRefClockMHz = 0;
      m_rampFpsSum     = 0.0;
      m_rampSampleCount    = 0;
      break;
    }

    if ( !stepComplete )
    {
      // Check for early instability (core: droop check; VRAM: no early check)
      bool earlyUnstable = false;
      if ( m_activeClk != AutoOCComponent::Vram )
      {
        earlyUnstable = ( m_stableSampleCount >= 6 && m_droopCount * 100 / m_stableSampleCount > 20 );
      }

      if ( earlyUnstable )
      {
        int doubleMargin = safetyMarginForComponent() * 2;
        int reduced = std::max( 0, m_bestStable - doubleMargin );
        reduced = floorToResolution( reduced );

        if ( reduced <= 0 || reduced >= m_mid )
        {
          enterDone( false, "Validation failed and no lower offset to try" );
          return;
        }

        log( "AutoOC: Validation unstable at +" + std::to_string( m_mid )
             + " MHz — retrying at +" + std::to_string( reduced ) + " MHz" );

        applyOffset( reduced );
        m_mid            = reduced;
        m_stepStart      = std::chrono::steady_clock::now();
        m_sampleCount    = 0;
        m_stableSampleCount = 0;
        m_droopCount     = 0;
        m_thermalCount   = 0;
        m_lowUtilCount   = 0;
        m_peakTempC      = 0;
        m_minClockMHz    = INT32_MAX;
        m_achievedClockMHz = 0;
        m_clockHistoryIdx   = 0;
        m_clockHistoryCount = 0;
        m_slidingRefClockMHz = 0;
      }
      else
      {
        emitProgress( "Validating +" + std::to_string( m_mid ) + " MHz ("
                      + std::to_string( elapsedMs / 1000 ) + "/"
                      + std::to_string( m_stepDurationMs / 1000 ) + " s)" );
      }
      break;
    }

    // Validation complete — check results
    bool stable;
    if ( m_activeClk == AutoOCComponent::Vram )
    {
      // VRAM validation: check that FPS stays near the ramp peak.
      // If FPS dropped below the peak by more than the regression
      // threshold, the offset is too high (ECC overhead).
      double avgFps = ( m_rampSampleCount > 0 )
        ? m_rampFpsSum / static_cast< double >( m_rampSampleCount ) : 0.0;
      double dropThresh = m_vramPeakFps * static_cast< double >( m_config.vramFpsDropPct ) / 100.0;
      stable = ( avgFps >= m_vramPeakFps - dropThresh );
      log( "AutoOC: VRAM validation — FPS=" + std::to_string( static_cast< int >( avgFps ) )
           + ", peak=" + std::to_string( static_cast< int >( m_vramPeakFps ) )
           + ", " + ( stable ? "STABLE" : "REGRESSED" ) );
    }
    else
    {
      // Core validation: droop-based
      stable = ( m_droopCount * 100 / std::max( m_stableSampleCount, 1 ) < 15 );
    }

    if ( stable )
    {
      // Store result for this component
      if ( m_activeClk == AutoOCComponent::Core )
      {
        m_finalCoreOffset = m_mid;
        log( "AutoOC: Core validated at +" + std::to_string( m_mid ) + " MHz" );

        // If scanning both, now do VRAM
        if ( m_component == AutoOCComponent::Both )
        {
          m_activeClk = AutoOCComponent::Vram;
          log( "AutoOC: Switching to VRAM scan (core stays at +" +
               std::to_string( m_finalCoreOffset ) + " MHz)" );
          // Keep core offset applied; baseline for VRAM
          enterBaseline();
          return;
        }
      }
      else
      {
        m_finalVramOffset = m_mid;
        log( "AutoOC: VRAM validated at +" + std::to_string( m_mid ) + " MHz" );
      }

      std::string summary = "Auto-OC complete:";
      if ( m_finalCoreOffset > 0 )
        summary += " core=+" + std::to_string( m_finalCoreOffset ) + " MHz";
      if ( m_finalVramOffset > 0 )
        summary += " vram=+" + std::to_string( m_finalVramOffset ) + " MHz";

      enterDone( true, summary );
    }
    else
    {
      // Validation failed — try with double margin
      int doubleMargin = safetyMarginForComponent() * 2;
      int reduced = std::max( 0, m_bestStable - doubleMargin );
      reduced = floorToResolution( reduced );

      if ( reduced <= 0 || reduced >= m_mid )
      {
        enterDone( false, "Validation failed — no safe offset found" );
        return;
      }

      log( "AutoOC: Validation failed at +" + std::to_string( m_mid )
           + " MHz — final attempt at +" + std::to_string( reduced ) + " MHz" );

      applyOffset( reduced );
      m_mid            = reduced;
      m_stepStart      = std::chrono::steady_clock::now();
      m_sampleCount    = 0;
      m_stableSampleCount = 0;
      m_droopCount     = 0;
      m_thermalCount   = 0;
      m_lowUtilCount   = 0;
      m_peakTempC      = 0;
      m_minClockMHz    = INT32_MAX;
      m_achievedClockMHz = 0;
      m_clockHistoryIdx   = 0;
      m_clockHistoryCount = 0;
      m_slidingRefClockMHz = 0;
      m_rampFpsSum     = 0.0;
      m_rampSampleCount    = 0;
    }
    break;
  }

  default:
    m_pollTimer->stop();
    break;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Stability sampling
// ─────────────────────────────────────────────────────────────────────────────

StabilityResult AutoOCWorker::sampleStability()
{
  ++m_sampleCount;

  auto elapsed = std::chrono::steady_clock::now() - m_stepStart;
  int elapsedMs = static_cast< int >(
    std::chrono::duration_cast< std::chrono::milliseconds >( elapsed ).count() );
  bool settling = ( elapsedMs < m_config.settleMs );

  // ── Temperature check (always active, even during settling) ──
  auto temp = m_nvml->getTemperatureDegC( m_deviceIndex );
  if ( temp.has_value() )
  {
    int t = static_cast< int >( *temp );
    m_peakTempC = std::max( m_peakTempC, t );

    if ( t >= m_config.thermalLimitC )
    {
      ++m_thermalCount;
      return StabilityResult::ThermalLimit;
    }
  }

  // ── Read current clock ──
  auto clk = m_nvml->getGpuClockMHz( m_deviceIndex );
  int c = 0;
  bool clockValid = false;
  if ( clk.has_value() )
  {
    c = static_cast< int >( *clk );
    clockValid = ( c > 0 );
    m_minClockMHz = std::min( m_minClockMHz, c );
  }

  // ── Baseline phase: accumulate clock samples for robust average ──
  if ( m_phase == AutoOCPhase::Baseline && !settling && clockValid )
  {
    if ( m_baselineClkSampleCount < BASELINE_MAX_SAMPLES )
    {
      m_baselineClkSamples[m_baselineClkSampleCount] = c;
      ++m_baselineClkSampleCount;
    }

    // For VRAM ramp: accumulate FPS as baseline performance metric
    if ( m_activeClk == AutoOCComponent::Vram && m_fpsServer )
    {
      double fps = m_fpsServer->currentFps();
      if ( fps > 0.0 )
      {
        m_rampFpsSum += fps;
        ++m_rampSampleCount;
      }
    }

    return StabilityResult::Stable;
  }

  // ── Settling phase: track peak achieved clock ──
  if ( settling && clockValid )
  {
    m_achievedClockMHz = std::max( m_achievedClockMHz, c );
    return StabilityResult::Stable;
  }

  // ── Post-settle stability check ──
  if ( !settling && m_phase != AutoOCPhase::Baseline )
  {
    // Dispatch to the appropriate stability algorithm
    if ( m_activeClk == AutoOCComponent::Vram )
    {
      return sampleStabilityVramRamp();
    }

    // GPU utilization check (core algorithms)
    auto util = m_nvml->getComputeUtilPct( m_deviceIndex );
    if ( util.has_value() )
    {
      if ( static_cast< int >( *util ) < m_config.minGpuUtilPct )
        ++m_lowUtilCount;
    }

    return sampleStabilityMaxOffset();
  }

  return StabilityResult::Stable;
}

// ─────────────────────────────────────────────────────────────────────────────
// VRAM ramp stability: accumulate FPS for regression detection
// ─────────────────────────────────────────────────────────────────────────────

StabilityResult AutoOCWorker::sampleStabilityVramRamp()
{
  if ( m_fpsServer )
  {
    double fps = m_fpsServer->currentFps();
    if ( fps > 0.0 )
    {
      m_rampFpsSum += fps;
      ++m_rampSampleCount;
    }
  }

  ++m_stableSampleCount;

  // No per-sample instability decision for VRAM — regression is evaluated
  // at step completion in onPollTick().
  return StabilityResult::Stable;
}

// ─────────────────────────────────────────────────────────────────────────────
// MaxOffset stability: sliding reference clock (legacy algorithm)
// ─────────────────────────────────────────────────────────────────────────────

StabilityResult AutoOCWorker::sampleStabilityMaxOffset()
{
  auto clk = m_nvml->getGpuClockMHz( m_deviceIndex );
  if ( !clk.has_value() )
  {
    ++m_stableSampleCount;
    ++m_droopCount;
    return StabilityResult::Unstable;
  }

  int c = static_cast< int >( *clk );

  // Seed the sliding reference from the settling peak if available,
  // or from the first valid post-settle sample.
  if ( m_slidingRefClockMHz == 0 )
    m_slidingRefClockMHz = ( m_achievedClockMHz > 0 ) ? m_achievedClockMHz : c;

  // Feed into the sliding history ring buffer.
  m_clockHistory[m_clockHistoryIdx] = c;
  m_clockHistoryIdx = ( m_clockHistoryIdx + 1 ) % CLOCK_HISTORY_SIZE;
  if ( m_clockHistoryCount < CLOCK_HISTORY_SIZE )
    ++m_clockHistoryCount;

  // Update the sliding reference: use the minimum of the recent history
  // window.  This tracks the GPU's natural thermal/power-driven clock
  // decline so that only *sudden* drops (instability) are flagged as
  // droop — not the normal 30-60 MHz drift over 15+ seconds from
  // NVIDIA GPU Boost stepping down as the die heats up.
  if ( m_clockHistoryCount >= 4 )
  {
    int recentMin = INT32_MAX;
    for ( int i = 0; i < m_clockHistoryCount; ++i )
      recentMin = std::min( recentMin, m_clockHistory[i] );

    // Allow the reference to drift downward with the GPU's natural
    // frequency cap, but never upward (a clock increase is not droop).
    m_slidingRefClockMHz = std::min( m_slidingRefClockMHz, recentMin + m_config.clockDroopMHz / 2 );
    // Floor at 80% of settled peak to avoid reference collapsing completely
    int floor = m_achievedClockMHz > 0 ? ( m_achievedClockMHz * 80 / 100 ) : 0;
    m_slidingRefClockMHz = std::max( m_slidingRefClockMHz, floor );
  }

  ++m_stableSampleCount;

  // Droop = sudden drop from the sliding reference clock.
  int droopThreshold = m_slidingRefClockMHz - m_config.clockDroopMHz;
  if ( c < droopThreshold )
    ++m_droopCount;

  // Throttle reason check — thermal limiters indicate instability.
  auto reason = m_nvml->getPerfLimitReason( m_deviceIndex );
  if ( reason.has_value() )
  {
    const auto &r = *reason;
    if ( r == "HW Thermal" || r == "SW Thermal" )
      ++m_droopCount;
  }

  return StabilityResult::Stable;
}

// ─────────────────────────────────────────────────────────────────────────────
// Offset application
// ─────────────────────────────────────────────────────────────────────────────

void AutoOCWorker::applyOffset( int offsetMHz )
{
  auto clockType = ( m_activeClk == AutoOCComponent::Vram )
                     ? nvml::NVML_CLOCK_MEM
                     : nvml::NVML_CLOCK_GRAPHICS;

  bool ok = m_nvml->setClockOffset( m_deviceIndex, clockType, nvml::NVML_PSTATE_0, offsetMHz );
  if ( !ok )
    log( "AutoOC: WARNING — failed to apply offset " + std::to_string( offsetMHz ) + " MHz" );
}

void AutoOCWorker::resetOffset()
{
  // Reset whichever component(s) we were scanning
  if ( m_component == AutoOCComponent::Core || m_component == AutoOCComponent::Both )
    m_nvml->setClockOffset( m_deviceIndex, nvml::NVML_CLOCK_GRAPHICS, nvml::NVML_PSTATE_0, 0 );
  if ( m_component == AutoOCComponent::Vram || m_component == AutoOCComponent::Both )
    m_nvml->setClockOffset( m_deviceIndex, nvml::NVML_CLOCK_MEM, nvml::NVML_PSTATE_0, 0 );
}

// ─────────────────────────────────────────────────────────────────────────────
// Utilities
// ─────────────────────────────────────────────────────────────────────────────

int AutoOCWorker::maxOffsetForComponent() const
{
  return ( m_activeClk == AutoOCComponent::Vram )
           ? NvmlOffsetCaps::VRAM_MAX_OFFSET
           : NvmlOffsetCaps::GPU_MAX_OFFSET;
}

int AutoOCWorker::safetyMarginForComponent() const
{
  return ( m_activeClk == AutoOCComponent::Vram )
           ? m_config.safetyMarginVramMHz
           : m_config.safetyMarginCoreMHz;
}

int AutoOCWorker::ceilToResolution( int value ) const
{
  int r = m_config.resolutionMHz;
  return ( ( value + r - 1 ) / r ) * r;
}

int AutoOCWorker::floorToResolution( int value ) const
{
  int r = m_config.resolutionMHz;
  return ( value / r ) * r;
}

void AutoOCWorker::log( const std::string &msg )
{
  syslog( LOG_INFO, "%s", msg.c_str() );
  if ( m_logFn )
    m_logFn( msg );
}

void AutoOCWorker::emitProgress( const std::string &msg )
{
  AutoOCProgress prog;
  prog.phase           = m_phase;
  prog.component       = m_activeClk;
  prog.iteration       = m_iteration;
  prog.maxIterations   = m_maxIterations;
  prog.currentOffsetMHz = m_mid;
  prog.bestStableMHz   = m_bestStable;
  prog.message         = msg;

  // Fill live metrics
  auto temp = m_nvml->getTemperatureDegC( m_deviceIndex );
  prog.tempC = static_cast< int >( temp.value_or( 0 ) );

  auto clk = m_nvml->getGpuClockMHz( m_deviceIndex );
  prog.gpuClockMHz = static_cast< int >( clk.value_or( 0 ) );

  auto memClk = m_nvml->getMemClockMHz( m_deviceIndex );
  prog.vramClockMHz = static_cast< int >( memClk.value_or( 0 ) );

  auto util = m_nvml->getComputeUtilPct( m_deviceIndex );
  prog.gpuUtilPct = static_cast< int >( util.value_or( 0 ) );

  prog.fps = m_fpsServer ? m_fpsServer->currentFps() : -1.0;

  // Update in-game overlay via shared memory
  {
    UccOverlayData od{};
    od.active          = 1;
    od.mode            = 0; // Auto-OC
    od.phase           = static_cast<uint8_t>( prog.phase );
    od.iteration       = prog.iteration;
    od.maxIterations   = prog.maxIterations;
    od.currentOffsetMHz = prog.currentOffsetMHz;
    od.bestStableMHz   = prog.bestStableMHz;
    od.gpuClockMHz     = prog.gpuClockMHz;
    od.vramClockMHz    = prog.vramClockMHz;
    od.tempC           = prog.tempC;
    od.gpuUtilPct      = prog.gpuUtilPct;
    od.fps             = prog.fps;
    od.lastResult      = static_cast<uint8_t>( prog.lastResult );
    if ( !msg.empty() )
      std::strncpy( od.message, msg.c_str(), sizeof( od.message ) - 1 );
    OverlayShmWriter::instance().update( od );
  }

  emit progress( prog );
}
