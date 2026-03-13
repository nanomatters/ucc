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

#include "workers/AutoUndervoltWorker.hpp"

#include "FpsServer.hpp"

#include <algorithm>
#include <syslog.h>

namespace
{
double filteredAverageFps( const std::vector< double > &samples )
{
  if ( samples.empty() )
    return -1.0;

  if ( samples.size() < 5 )
  {
    double sum = 0.0;
    for ( const double v : samples )
      sum += v;
    return sum / static_cast< double >( samples.size() );
  }

  std::vector< double > sorted = samples;
  std::sort( sorted.begin(), sorted.end() );

  const size_t n = sorted.size();
  const double median = ( n % 2 == 0 )
    ? ( sorted[n / 2 - 1] + sorted[n / 2] ) * 0.5
    : sorted[n / 2];

  // Drop obvious transient outliers around median (frame hitches / spikes).
  const double lower = median * 0.85;
  const double upper = median * 1.15;
  double filteredSum = 0.0;
  size_t filteredCount = 0;
  for ( const double v : sorted )
  {
    if ( v >= lower && v <= upper )
    {
      filteredSum += v;
      ++filteredCount;
    }
  }

  if ( filteredCount >= std::max< size_t >( 5, n / 2 ) )
    return filteredSum / static_cast< double >( filteredCount );

  // Fallback to trimmed mean when filtering became too aggressive.
  const size_t trim = n / 10;
  const size_t begin = trim;
  const size_t end = n - trim;
  if ( begin >= end )
    return median;

  double trimmedSum = 0.0;
  for ( size_t i = begin; i < end; ++i )
    trimmedSum += sorted[i];
  return trimmedSum / static_cast< double >( end - begin );
}

double filteredRunningAverageFps( const std::vector< double > &samples,
                                  int pollIntervalMs,
                                  int windowMs )
{
  if ( samples.empty() )
    return -1.0;

  const int safePollMs = std::max( 1, pollIntervalMs );
  const int safeWindowMs = std::max( safePollMs, windowMs );
  const size_t windowSamples = static_cast< size_t >( std::max( 1, safeWindowMs / safePollMs ) );

  if ( samples.size() <= windowSamples )
    return filteredAverageFps( samples );

  std::vector< double > tail;
  tail.reserve( windowSamples );
  const size_t start = samples.size() - windowSamples;
  for ( size_t i = start; i < samples.size(); ++i )
    tail.push_back( samples[i] );

  return filteredAverageFps( tail );
}
}

// ─────────────────────────────────────────────────────────────────────────────
// Construction / destruction
// ─────────────────────────────────────────────────────────────────────────────

AutoUndervoltWorker::AutoUndervoltWorker( std::shared_ptr< NvmlWrapper > nvml,
                                          LogFn logFn,
                                          QObject *parent )
  : QObject( parent )
  , m_nvml( std::move( nvml ) )
  , m_logFn( std::move( logFn ) )
{
  m_pollTimer = new QTimer( this );
  m_pollTimer->setTimerType( Qt::PreciseTimer );
  connect( m_pollTimer, &QTimer::timeout, this, &AutoUndervoltWorker::onPollTick );
}

AutoUndervoltWorker::~AutoUndervoltWorker()
{
  if ( isRunning() )
  {
    m_stopRequested = true;
    m_pollTimer->stop();
    restoreOriginalState( false );
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

bool AutoUndervoltWorker::isAvailable() const
{
  return m_nvml && m_nvml->isAvailable() && m_nvml->deviceCount() > 0;
}

bool AutoUndervoltWorker::start( unsigned int deviceIndex,
                                 const UndervoltConfig &config )
{
  if ( !isAvailable() )
  {
    log( "AutoUV: NVML not available" );
    return false;
  }

  if ( isRunning() )
  {
    log( "AutoUV: already running" );
    return false;
  }

  if ( !m_fpsServer || !m_fpsServer->isRunning() )
  {
    // Try to start the FPS server if not already running.
    if ( m_fpsServer )
    {
      if ( !m_fpsServer->start() )
      {
        log( "AutoUV: FPS server unavailable" );
        return false;
      }
    }
    else
    {
      log( "AutoUV: no FPS server bound" );
      return false;
    }
  }

  // We need a connected FPS client to know which app we're undervolting.
  // If no client is connected yet, we'll detect it during baseline.
  m_config      = config;
  m_deviceIndex = deviceIndex;
  m_stopRequested = false;

  m_appName = m_fpsServer->clientAppName();
  if ( m_appName.empty() )
    log( "AutoUV: no FPS client connected yet — will detect during baseline" );
  else
    log( "AutoUV: starting scan for app='" + m_appName + "'" );

  // Snapshot current limits/locks so we can safely restore on cancellation
  // or failed validation.
  captureOriginalState();

  enterBaseline();
  return true;
}

void AutoUndervoltWorker::stop()
{
  if ( !isRunning() )
    return;
  m_stopRequested = true;
  log( "AutoUV: stop requested" );
}

bool AutoUndervoltWorker::isRunning() const
{
  return m_phase != UVPhase::Idle && m_phase != UVPhase::Done;
}

std::optional< AppUndervoltProfile >
AutoUndervoltWorker::getAppProfile( const std::string &appName ) const
{
  auto it = m_appProfiles.find( appName );
  if ( it != m_appProfiles.end() )
    return it->second;
  return std::nullopt;
}

bool AutoUndervoltWorker::applyAppProfile( const std::string &appName,
                                           unsigned int deviceIndex )
{
  auto it = m_appProfiles.find( appName );
  if ( it == m_appProfiles.end() )
    return false;

  int cap = it->second.gpuFreqCapMHz;
  if ( cap <= 0 )
    return false;

  const int coreOffset = std::max( 0, it->second.coreOffsetMHz );
  (void) m_nvml->setClockOffset( deviceIndex, nvml::NVML_CLOCK_GRAPHICS, nvml::NVML_PSTATE_0, coreOffset );

  bool ok = m_nvml->setGpuLockedClocks( deviceIndex,
                                         static_cast< unsigned int >( cap ),
                                         static_cast< unsigned int >( cap ) );
  if ( ok )
  {
    it->second.lastUsed = std::chrono::system_clock::now();
    log( "AutoUV: applied stored profile for '" + appName
      + "' — cap=" + std::to_string( cap ) + " MHz, coreOffset=+"
      + std::to_string( coreOffset ) + " MHz" );
  }
  return ok;
}

void AutoUndervoltWorker::removeAppProfile( const std::string &appName )
{
  m_appProfiles.erase( appName );
}

void AutoUndervoltWorker::clearAllProfiles()
{
  m_appProfiles.clear();
}

void AutoUndervoltWorker::loadProfiles(
  const std::map< std::string, AppUndervoltProfile > &profiles )
{
  m_appProfiles = profiles;
}

// ─────────────────────────────────────────────────────────────────────────────
// State machine transitions
// ─────────────────────────────────────────────────────────────────────────────

void AutoUndervoltWorker::enterBaseline()
{
  m_phase = UVPhase::Baseline;

  // Reset any existing OC profile settings so baseline is measured from
  // a clean state (no prior offsets or locked clocks influencing reading).
  resetFreqCap();
  applyCoreOffset( 0 );

  m_stepStart      = std::chrono::steady_clock::now();
  m_stepDurationMs = m_config.baselineMs;
  m_sampleCount    = 0;
  m_lowUtilCount   = 0;
  m_peakTempC      = 0;
  m_pstateSamples  = 0;
  m_p0Samples      = 0;

  m_baselineClockMHz = 0;
  m_baselineFps      = 0.0;
  m_targetFps        = 0.0;
  m_fpsThreshold     = 0.0;
  m_peakFpsObserved  = 0.0;
  m_minFpsObserved   = -1.0;
  m_baselinePowerW   = 0.0;
  m_baselineVoltageMv = 0.0;
  m_baselineTempC    = 0;

  m_fpsAccum   = 0.0;
  m_fpsSamples = 0;
  m_baselineFpsSamples.clear();
  m_stepFpsSamples.clear();
  m_powerAccum   = 0.0;
  m_powerSamples = 0;
  m_voltageAccum = 0.0;
  m_voltageSamples = 0;
  m_clkSampleCount = 0;

  log( "AutoUV: Phase 0 — Baseline (" + std::to_string( m_config.baselineMs / 1000 ) + " s)" );
  emitProgress( "Measuring baseline (keep workload running)..." );

  m_pollTimer->start( m_config.pollIntervalMs );
}

void AutoUndervoltWorker::enterSearch()
{
  m_phase = UVPhase::Searching;
  ensureMaxPowerLimit();

  m_iteration = 0;
  m_maxIterations = std::max( 1,
    m_config.maxCoreOffsetMHz / std::max( 1, m_config.offsetStepMHz ) );

  m_stableCapMHz = m_baselineClockMHz;
  m_stableOffsetMHz = 0;
  m_searchCapMHz = m_stableCapMHz;
  m_searchOffsetMHz = m_stableOffsetMHz;
  m_bestCap = m_stableCapMHz;
  m_bestOffset = m_stableOffsetMHz;

  m_blockedLowerCap = true;
  m_blockedRaiseOffset = false;
  m_lastAction = UVSearchAction::None;
  m_nextAction = UVSearchAction::RaiseOffset;
  m_stepRetryCount = 0;

  applyFreqCap( m_baselineClockMHz );
  applyCoreOffset( 0 );

  log( "AutoUV: Phase 1 — fixed-cap offset sweep, cap="
       + std::to_string( m_searchCapMHz ) + " MHz" );

  // Jump straight to first offset candidate at fixed baseline cap.
  if ( !startNextSearchCandidate( UVSearchAction::RaiseOffset ) )
  {
    // Extremely unlikely: can't even try the first candidate.
    enterDone( false, "Cannot start search — offset and cap both blocked" );
    return;
  }
}

void AutoUndervoltWorker::enterOffsetSearch()
{
  // Kept for compatibility; v2 algorithm searches cap+offset jointly.
  enterValidation();
}

void AutoUndervoltWorker::enterValidation()
{
  m_phase = UVPhase::Validating;

  m_finalCapMHz = m_bestCap + m_config.safetyMarginMHz;
  if ( m_finalCapMHz > m_baselineClockMHz )
    m_finalCapMHz = m_baselineClockMHz;

  m_mid = ( m_finalCapMHz > 0 ) ? m_finalCapMHz : m_bestCap;
  if ( m_mid <= 0 )
    m_mid = m_baselineClockMHz;

  log( "AutoUV: Phase 3 — Validation at cap=" + std::to_string( m_mid )
       + " MHz, coreOffset=+" + std::to_string( m_bestOffset ) + " MHz" );

  applyFreqCap( m_mid );
  applyCoreOffset( m_bestOffset );

  resetStepMetrics();
  m_stepDurationMs = m_config.validationMs;

  emitProgress( "Validating cap " + std::to_string( m_mid ) + " MHz / offset +"
                + std::to_string( m_bestOffset ) + " MHz ("
                + std::to_string( m_config.validationMs / 1000 ) + " s)" );
}

void AutoUndervoltWorker::enterDone( bool success, const std::string &msg )
{
  m_pollTimer->stop();
  m_phase = UVPhase::Done;

  if ( !success )
    restoreOriginalState( false );
  else
    restoreOriginalState( true );

  // Compute power saving estimate
  double powerSavedPct = 0.0;
  double finalAvgPower = 0.0;
  double finalAvgVoltage = 0.0;
  if ( success && m_baselinePowerW > 0.0 && m_powerSamples > 0 )
  {
    finalAvgPower = m_powerAccum / static_cast< double >( m_powerSamples );
    powerSavedPct = ( ( m_baselinePowerW - finalAvgPower ) / m_baselinePowerW ) * 100.0;
    if ( powerSavedPct < 0.0 ) powerSavedPct = 0.0;
  }
  if ( success && m_voltageSamples > 0 )
    finalAvgVoltage = m_voltageAccum / static_cast< double >( m_voltageSamples );

  double finalFps = 0.0;
  if ( success )
  {
    finalFps = filteredRunningAverageFps( m_stepFpsSamples,
                                          m_config.pollIntervalMs,
                                          m_config.fpsCompareWindowMs );
  }
  else if ( m_fpsServer && m_fpsServer->currentFps() > 0.0 )
    finalFps = m_fpsServer->currentFps();

  // Store per-app profile on success.
  if ( success && !m_appName.empty() )
  {
    AppUndervoltProfile profile;
    profile.appName        = m_appName;
    profile.gpuFreqCapMHz  = m_mid;
        profile.coreOffsetMHz  = m_bestOffset;
    profile.baselineClkMHz = m_baselineClockMHz;
    profile.baselineFps    = m_baselineFps;
    profile.achievedFps    = finalFps;
        profile.achievedPowerW = finalAvgPower;
        profile.achievedVoltageMv = finalAvgVoltage;
    profile.lastUsed       = std::chrono::system_clock::now();
    m_appProfiles[ m_appName ] = profile;

    log( "AutoUV: stored profile for '" + m_appName + "' — cap="
          + std::to_string( m_mid ) + " MHz, coreOffset=+" + std::to_string( m_bestOffset ) + " MHz, FPS="
         + std::to_string( static_cast< int >( finalFps ) )
         + ", power saved ~" + std::to_string( static_cast< int >( powerSavedPct ) ) + "%" );
  }

  log( "AutoUV: " + msg );

  UndervoltResult result;
  result.gpuFreqCapMHz  = success ? m_mid : 0;
  result.coreOffsetMHz  = success ? m_bestOffset : 0;
  result.baselineClkMHz = m_baselineClockMHz;
  result.baselineFps    = m_baselineFps;
  result.finalFps       = finalFps;
  result.finalPowerW    = finalAvgPower;
  result.baselineVoltageMv = m_baselineVoltageMv;
  result.finalVoltageMv = finalAvgVoltage;
  result.powerSavedPct  = powerSavedPct;
  result.success        = success;
  result.appName        = m_appName;
  result.message        = msg;

  emitProgress( msg );
  emit finished( result );
}

// ─────────────────────────────────────────────────────────────────────────────
// Poll tick — the heart of the state machine
// ─────────────────────────────────────────────────────────────────────────────

void AutoUndervoltWorker::onPollTick()
{
  // Drain any pending FPS data.
  if ( m_fpsServer ) m_fpsServer->poll();

  if ( m_stopRequested )
  {
    enterDone( false, "Scan cancelled by user" );
    return;
  }

  auto elapsed = std::chrono::steady_clock::now() - m_stepStart;
  int elapsedMs = static_cast< int >(
    std::chrono::duration_cast< std::chrono::milliseconds >( elapsed ).count() );
  bool inSettling = ( elapsedMs < m_config.settleMs );
  bool stepComplete = ( elapsedMs >= m_stepDurationMs );

  // ── Sample GPU metrics ──
  auto tempOpt  = m_nvml->getTemperatureDegC( m_deviceIndex );
  auto clkOpt   = m_nvml->getGpuClockMHz( m_deviceIndex );
  auto utilOpt  = m_nvml->getComputeUtilPct( m_deviceIndex );
  auto powerOpt = m_nvml->getPowerDrawW( m_deviceIndex );
  auto voltOpt  = m_nvml->getCoreVoltageMv( m_deviceIndex );
  auto pstateOpt = m_nvml->getCurrentPstate( m_deviceIndex );
  auto perfLimitReasonOpt = m_nvml->getPerfLimitReason( m_deviceIndex );
  auto throttleMaskOpt = m_nvml->getCurrentClocksThrottleReasonsMask( m_deviceIndex );
  auto eventMaskOpt = m_nvml->getCurrentClocksEventReasonsMask( m_deviceIndex );
  auto powerViolationOpt = m_nvml->getPerfPolicyViolationUsec( m_deviceIndex,
                                                               nvml::NVML_PERF_POLICY_POWER );
  auto thermalViolationOpt = m_nvml->getPerfPolicyViolationUsec( m_deviceIndex,
                                                                 nvml::NVML_PERF_POLICY_THERMAL );
  auto nvapiPerfLimiterOpt = m_nvml->getNvapiPerfLimiterMask( m_deviceIndex );
  auto nvapiGfxClockOpt = m_nvml->getNvapiCurrentGraphicsClockMHz( m_deviceIndex );
  auto nvapiPowerBudgetOpt = m_nvml->getNvapiClientPowerBudgetW( m_deviceIndex );

  int  tempC    = static_cast< int >( tempOpt.value_or( 0 ) );
  int  gpuClk   = static_cast< int >( clkOpt.value_or( 0 ) );
  int  gpuUtil  = static_cast< int >( utilOpt.value_or( 0 ) );
  double powerW = powerOpt.value_or( 0.0 );
  double voltageMv = static_cast< double >( voltOpt.value_or( 0 ) );
  const bool inP0 = pstateOpt.has_value() && *pstateOpt == 0U;

  if ( tempC > m_peakTempC ) m_peakTempC = tempC;
  ++m_sampleCount;

  // Thermal hard-stop
  if ( tempC >= m_config.thermalLimitC )
  {
    enterDone( false, "GPU thermal limit (" + std::to_string( tempC ) + " °C)" );
    return;
  }

  // Track low-util samples
  if ( gpuUtil < m_config.minGpuUtilPct )
    ++m_lowUtilCount;

  if ( pstateOpt.has_value() )
  {
    ++m_pstateSamples;
    if ( inP0 )
      ++m_p0Samples;
  }

  if ( !inSettling && m_phase != UVPhase::Baseline )
  {
    ++m_postSettleSamples;

    if ( gpuClk > 0 )
    {
      if ( m_slidingRefClockMHz == 0 )
        m_slidingRefClockMHz = gpuClk;

      m_clockHistory[m_clockHistoryIdx] = gpuClk;
      m_clockHistoryIdx = ( m_clockHistoryIdx + 1 ) % CLOCK_HISTORY_SIZE;
      if ( m_clockHistoryCount < CLOCK_HISTORY_SIZE )
        ++m_clockHistoryCount;

      if ( m_clockHistoryCount >= 4 )
      {
        int recentMin = INT32_MAX;
        for ( int i = 0; i < m_clockHistoryCount; ++i )
          recentMin = std::min( recentMin, m_clockHistory[i] );

        m_slidingRefClockMHz = std::min( m_slidingRefClockMHz,
                                         recentMin + m_config.clockDroopMHz / 2 );
        const int floor = ( m_baselineClockMHz > 0 ) ? ( m_baselineClockMHz * 65 / 100 ) : 0;
        m_slidingRefClockMHz = std::max( m_slidingRefClockMHz, floor );
      }

      bool droopedThisSample = ( gpuClk < ( m_slidingRefClockMHz - m_config.clockDroopMHz ) );
      if ( nvapiGfxClockOpt.has_value() )
      {
        const int nvapiClk = static_cast< int >( *nvapiGfxClockOpt );
        if ( nvapiClk > 0 && nvapiClk < ( m_slidingRefClockMHz - m_config.clockDroopMHz ) )
          droopedThisSample = true;
      }

      if ( droopedThisSample )
        ++m_droopCount;
    }

    if ( !throttleMaskOpt.has_value() && perfLimitReasonOpt.has_value() )
    {
      const std::string &reason = *perfLimitReasonOpt;
      if ( reason == "Power Limit" )
        ++m_powerCapCount;
      else if ( reason == "HW Thermal" || reason == "SW Thermal" || reason == "HW Slowdown" )
        ++m_thermalThrottleCount;
    }

    if ( throttleMaskOpt.has_value() )
    {
      const auto mask = static_cast< nvml::nvmlClocksThrottleReasons_t >( *throttleMaskOpt );
      if ( mask & nvml::NVML_CLOCKS_THROTTLE_REASON_SW_POWER_CAP )
        ++m_powerCapCount;
      if ( mask & ( nvml::NVML_CLOCKS_THROTTLE_REASON_HW_THERMAL_SLOWDOWN
                  | nvml::NVML_CLOCKS_THROTTLE_REASON_SW_THERMAL_SLOWDOWN
                  | nvml::NVML_CLOCKS_THROTTLE_REASON_HW_SLOWDOWN ) )
        ++m_thermalThrottleCount;
    }

    if ( eventMaskOpt.has_value() )
    {
      const auto mask = static_cast< nvml::nvmlClocksEventReasons_t >( *eventMaskOpt );
      if ( mask & nvml::NVML_CLOCKS_THROTTLE_REASON_SW_POWER_CAP )
        ++m_eventPowerCount;
      if ( mask & ( nvml::NVML_CLOCKS_THROTTLE_REASON_HW_THERMAL_SLOWDOWN
                  | nvml::NVML_CLOCKS_THROTTLE_REASON_SW_THERMAL_SLOWDOWN
                  | nvml::NVML_CLOCKS_THROTTLE_REASON_HW_SLOWDOWN
                  | nvml::NVML_CLOCKS_THROTTLE_REASON_HW_POWER_BRAKE_SLOWDOWN ) )
        ++m_eventThermalCount;
    }

    if ( powerViolationOpt.has_value() )
    {
      if ( m_prevPowerViolationUsec.has_value() && *powerViolationOpt > *m_prevPowerViolationUsec )
        ++m_violationPowerDeltaCount;
      m_prevPowerViolationUsec = *powerViolationOpt;
    }

    if ( thermalViolationOpt.has_value() )
    {
      if ( m_prevThermalViolationUsec.has_value() && *thermalViolationOpt > *m_prevThermalViolationUsec )
        ++m_violationThermalDeltaCount;
      m_prevThermalViolationUsec = *thermalViolationOpt;
    }

    if ( nvapiPerfLimiterOpt.has_value() && *nvapiPerfLimiterOpt != 0U )
      ++m_nvapiLimiterCount;

    if ( nvapiPowerBudgetOpt.has_value() )
    {
      const double budgetW = static_cast< double >( *nvapiPowerBudgetOpt );
      if ( budgetW > 0.0 && powerW > ( budgetW * 1.02 ) )
        ++m_powerCapCount;
    }
  }

  // Collect post-settle samples for FPS / power / clock
  if ( !inSettling || m_phase == UVPhase::Baseline )
  {
    double fps = ( m_fpsServer ) ? m_fpsServer->currentFps() : -1.0;
    if ( fps > 0.0 )
    {
      m_fpsAccum += fps;
      ++m_fpsSamples;
      if ( m_phase == UVPhase::Baseline )
        m_baselineFpsSamples.push_back( fps );
      else
        m_stepFpsSamples.push_back( fps );
      if ( fps > m_peakFpsObserved )
        m_peakFpsObserved = fps;
      if ( m_minFpsObserved < 0.0 || fps < m_minFpsObserved )
        m_minFpsObserved = fps;
    }
    if ( powerW > 0.0 )
    {
      m_powerAccum += powerW;
      ++m_powerSamples;
    }
    if ( voltageMv > 0.0 )
    {
      m_voltageAccum += voltageMv;
      ++m_voltageSamples;
    }
  }

  // Try to detect the app name during baseline if not yet known.
  if ( m_appName.empty() && m_fpsServer )
  {
    m_appName = m_fpsServer->clientAppName();
    if ( !m_appName.empty() )
      log( "AutoUV: detected app='" + m_appName + "'" );
  }

  // ── Phase dispatch ──
  switch ( m_phase )
  {
  // ────────────────────────────────────────────────────────────────────────
  case UVPhase::Baseline:
  {
    // Accumulate clock samples after settling
    if ( elapsedMs >= m_config.settleMs && gpuClk > 0 &&
         m_clkSampleCount < MAX_CLK_SAMPLES )
    {
      m_clkSamples[ m_clkSampleCount++ ] = gpuClk;
    }

    if ( stepComplete )
    {
      // ── Compute baseline clock (P25 — conservative sustained clock) ──
      if ( m_clkSampleCount >= 4 )
      {
        std::sort( m_clkSamples, m_clkSamples + m_clkSampleCount );
        int p25idx = m_clkSampleCount / 4;
        m_baselineClockMHz = m_clkSamples[ p25idx ];
      }
      else if ( gpuClk > 0 )
      {
        m_baselineClockMHz = gpuClk;
      }
      else
      {
        enterDone( false, "Could not read baseline GPU clock" );
        return;
      }

      // Baseline FPS
      m_baselineFps = filteredRunningAverageFps( m_baselineFpsSamples,
                     m_config.pollIntervalMs,
                     m_config.fpsCompareWindowMs );
      m_targetFps = m_baselineFps;
      m_fpsThreshold = m_targetFps * ( 1.0 - m_config.fpsDropPct / 100.0 );

      // Baseline power
      m_baselinePowerW = ( m_powerSamples > 0 )
        ? m_powerAccum / static_cast< double >( m_powerSamples ) : 0.0;
      m_baselineVoltageMv = ( m_voltageSamples > 0 )
        ? m_voltageAccum / static_cast< double >( m_voltageSamples ) : 0.0;

      m_baselineTempC = tempC;

      if ( m_targetFps <= 0.0 )
      {
        enterDone( false, "No FPS data received during baseline — is ucc-fps-layer active with UCC_FPS_HOOK=1?" );
        return;
      }

      // Baseline workload must be meaningfully GPU-bound, otherwise the search
      // can accept unstable points due to low/noisy utilization.
      const bool validBaselineWorkload = ( m_sampleCount > 0 )
        && ( m_lowUtilCount * 100 / m_sampleCount < 50 );
      if ( !validBaselineWorkload )
      {
        enterDone( false, "Baseline workload not GPU-bound enough for reliable undervolt scan" );
        return;
      }

      if ( m_appName.empty() )
      {
        enterDone( false, "No FPS client connected — cannot identify application" );
        return;
      }

      log( "AutoUV: Baseline — clock=" + std::to_string( m_baselineClockMHz )
           + " MHz, FPS=" + std::to_string( static_cast< int >( m_baselineFps ) )
         + " avg / " + std::to_string( static_cast< int >( m_peakFpsObserved ) ) + " peak"
           + ", threshold=" + std::to_string( static_cast< int >( m_fpsThreshold ) )
           + ", power=" + std::to_string( static_cast< int >( m_baselinePowerW ) )
           + " W, voltage=" + std::to_string( static_cast< int >( m_baselineVoltageMv ) )
           + " mV, temp=" + std::to_string( m_baselineTempC ) + " °C"
           + ", app='" + m_appName + "'" );

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
  case UVPhase::Searching:
  {
    // Detect app crash mid-step: if the FPS client disappears while testing
    // an offset, the offset killed the game — immediately flag unstable.
    if ( !stepComplete && m_fpsServer && m_fpsServer->clientPid() == 0
         && !m_appName.empty() && elapsedMs > m_config.settleMs )
    {
      log( "AutoUV: FPS client lost during offset=+" + std::to_string( m_searchOffsetMHz )
           + " MHz — app likely crashed, reverting to stable offset +"
           + std::to_string( m_stableOffsetMHz ) + " MHz" );

      m_blockedRaiseOffset = true;
      m_searchCapMHz = m_baselineClockMHz;
      m_searchOffsetMHz = m_stableOffsetMHz;
      applyFreqCap( m_searchCapMHz );
      applyCoreOffset( m_searchOffsetMHz );
      enterValidation();
      break;
    }

    if ( !stepComplete )
    {
      emitProgress( "Cap " + std::to_string( m_searchCapMHz ) + " MHz / offset +"
                    + std::to_string( m_searchOffsetMHz ) + " MHz ("
                    + std::to_string( elapsedMs / 1000 ) + "/"
                    + std::to_string( m_stepDurationMs / 1000 ) + " s)" );
      break;
    }

    double avgFps = 0.0;
    bool validWorkload = false;
    int p0ResidencyPct = 0;
    int droopPct = 0;
    int powerCapPct = 0;
    int thermalThrottlePct = 0;
    int eventPowerPct = 0;
    int eventThermalPct = 0;
    int violationPowerPct = 0;
    int violationThermalPct = 0;
    int nvapiLimiterPct = 0;
    double minFps = 0.0;
    // evaluateStep populates all metrics; we log them all for diagnostics
    // but during search we only gate on P0 + droop (actual OC instability).
    // Power/thermal/event/violation gates reflect normal GPU power management
    // that is identical at baseline — they don't indicate offset problems.
    (void) evaluateStep( avgFps, validWorkload, p0ResidencyPct,
                      droopPct, powerCapPct, thermalThrottlePct,
                      eventPowerPct, eventThermalPct,
                      violationPowerPct, violationThermalPct,
                      nvapiLimiterPct, minFps );

    // P-state residency is noisy/architecture-dependent when clocks are locked.
    // Keep it as telemetry but gate offset stability primarily on clock droop.
    const bool stepOk = !validWorkload
      || ( droopPct <= m_config.maxDroopPct );

    log( "AutoUV: offset=+" + std::to_string( m_searchOffsetMHz )
         + " MHz — P0=" + std::to_string( p0ResidencyPct )
         + "%, droop=" + std::to_string( droopPct )
         + "% [gates], pwrCap=" + std::to_string( powerCapPct )
         + "%, thermal=" + std::to_string( thermalThrottlePct )
         + "%, evtPwr=" + std::to_string( eventPowerPct )
         + "%, evtThm=" + std::to_string( eventThermalPct )
         + "%, vioPwr=" + std::to_string( violationPowerPct )
         + "%, vioThm=" + std::to_string( violationThermalPct )
         + "%, nvapiLim=" + std::to_string( nvapiLimiterPct )
         + "% [info] => " + ( stepOk ? "STABLE" : "UNSTABLE" ) );

          if ( !stepOk )
          {
         log( "AutoUV: unstable reason at offset +" + std::to_string( m_searchOffsetMHz )
           + " MHz: droop=" + std::to_string( droopPct )
           + "% > maxDroopPct=" + std::to_string( m_config.maxDroopPct )
           + "% (P0=" + std::to_string( p0ResidencyPct ) + "%, informational)" );
          }

    if ( stepOk )
    {
      m_stableCapMHz = m_baselineClockMHz;
      m_stableOffsetMHz = m_searchOffsetMHz;
      m_bestCap = m_stableCapMHz;
      m_bestOffset = m_stableOffsetMHz;

      ++m_iteration;

      if ( !startNextSearchCandidate( UVSearchAction::RaiseOffset ) )
      {
        enterValidation();
        return;
      }

      break;
    }

    // Unstable — this is the offset limit. No retries: go straight to validation
    // at the last known-stable offset.
    m_blockedRaiseOffset = true;
    m_searchCapMHz = m_baselineClockMHz;
    m_searchOffsetMHz = m_stableOffsetMHz;
    applyFreqCap( m_searchCapMHz );
    applyCoreOffset( m_searchOffsetMHz );

    enterValidation();

    break;
  }

  // ────────────────────────────────────────────────────────────────────────
  case UVPhase::OffsetSearching:
  {
    enterValidation();
    break;
  }

  // ────────────────────────────────────────────────────────────────────────
  case UVPhase::Validating:
  {
    if ( !stepComplete )
    {
      emitProgress( "Validating " + std::to_string( m_mid ) + " MHz / offset +"
                    + std::to_string( m_bestOffset ) + " MHz ("
                    + std::to_string( elapsedMs / 1000 ) + "/"
                    + std::to_string( m_stepDurationMs / 1000 ) + " s)" );
      break;
    }

    double avgFps = 0.0;
    bool validWorkload = false;
    int p0ResidencyPct = 0;
    int droopPct = 0;
    int powerCapPct = 0;
    int thermalThrottlePct = 0;
    int eventPowerPct = 0;
    int eventThermalPct = 0;
    int violationPowerPct = 0;
    int violationThermalPct = 0;
    int nvapiLimiterPct = 0;
    double minFps = 0.0;
    // Validation gates on the same OC-instability signals as search:
    // P0 residency and clock droop. Power/thermal are normal laptop behaviour.
    (void) evaluateStep( avgFps, validWorkload, p0ResidencyPct,
                      droopPct, powerCapPct, thermalThrottlePct,
                      eventPowerPct, eventThermalPct,
                      violationPowerPct, violationThermalPct,
                      nvapiLimiterPct, minFps );
    // Same stability gate as search: do not fail validation based on P-state.
    const bool stable = !validWorkload
      || ( droopPct <= m_config.maxDroopPct );
    if ( !stable )
    {
       log( "AutoUV: validation failed — FPS=" + std::to_string( static_cast< int >( avgFps ) )
         + " (target=" + std::to_string( static_cast< int >( m_targetFps ) )
         + ", threshold=" + std::to_string( static_cast< int >( m_fpsThreshold ) )
          + ", min=" + std::to_string( static_cast< int >( minFps ) )
          + "), P0=" + std::to_string( p0ResidencyPct )
          + "%, droop=" + std::to_string( droopPct )
          + "%, pwrCap=" + std::to_string( powerCapPct )
          + "%, thermal=" + std::to_string( thermalThrottlePct )
          + "%, evtPwr=" + std::to_string( eventPowerPct )
          + "%, evtThm=" + std::to_string( eventThermalPct )
          + "%, vioPwr=" + std::to_string( violationPowerPct )
          + "%, vioThm=" + std::to_string( violationThermalPct )
          + "%, nvapiLim=" + std::to_string( nvapiLimiterPct ) + "%" );
      log( "AutoUV: validation unstable reason: droop=" + std::to_string( droopPct )
           + "% > maxDroopPct=" + std::to_string( m_config.maxDroopPct )
           + "% (P0=" + std::to_string( p0ResidencyPct ) + "%, informational)" );
      enterDone( false, "Validation failed: unstable at cap "
                 + std::to_string( m_mid ) + " MHz" );
      return;
    }

    // FPS is used only here to tune final cap after offset sweep:
    // if the offset result still runs above baseline, reduce cap toward baseline FPS.
    const double targetFps = std::max( 1.0, m_targetFps );
    const double highBand = targetFps * 1.02;
    const double lowBand = targetFps * ( 1.0 - m_config.fpsDropPct / 100.0 );
    const int capStep = std::max( 1, m_config.stepMHz );
    const int minCapMHz = std::max( capStep, m_baselineClockMHz / 2 );

    if ( avgFps > highBand && ( m_mid - capStep ) >= minCapMHz )
    {
      const int nextCap = m_mid - capStep;
      log( "AutoUV: validation tuning — FPS above baseline ("
           + std::to_string( static_cast< int >( avgFps ) ) + ">"
           + std::to_string( static_cast< int >( highBand ) )
           + "), lowering cap to " + std::to_string( nextCap )
           + " MHz at offset +" + std::to_string( m_bestOffset ) + " MHz" );

      m_mid = nextCap;
      m_finalCapMHz = nextCap;
      applyFreqCap( m_mid );
      applyCoreOffset( m_bestOffset );
      resetStepMetrics();
      emitProgress( "Tuning cap " + std::to_string( m_mid ) + " MHz / offset +"
                    + std::to_string( m_bestOffset ) + " MHz" );
      break;
    }

    if ( avgFps < lowBand && ( m_mid + capStep ) <= m_baselineClockMHz )
    {
      const int recoverCap = m_mid + capStep;
      log( "AutoUV: validation tuning — FPS below target band ("
           + std::to_string( static_cast< int >( avgFps ) ) + "<"
           + std::to_string( static_cast< int >( lowBand ) )
           + "), raising cap to " + std::to_string( recoverCap ) + " MHz" );

      m_mid = recoverCap;
      m_finalCapMHz = recoverCap;
      applyFreqCap( m_mid );
      applyCoreOffset( m_bestOffset );
      resetStepMetrics();
      emitProgress( "Recovering cap " + std::to_string( m_mid ) + " MHz / offset +"
                    + std::to_string( m_bestOffset ) + " MHz" );
      break;
    }

    double avgPower = ( m_powerSamples > 0 )
      ? m_powerAccum / static_cast< double >( m_powerSamples ) : m_baselinePowerW;
    double savedPct = ( m_baselinePowerW > 0.0 )
      ? ( ( m_baselinePowerW - avgPower ) / m_baselinePowerW ) * 100.0 : 0.0;
    double avgVoltage = ( m_voltageSamples > 0 )
      ? m_voltageAccum / static_cast< double >( m_voltageSamples ) : 0.0;
    double voltSavedPct = ( m_baselineVoltageMv > 0.0 && avgVoltage > 0.0 )
      ? ( ( m_baselineVoltageMv - avgVoltage ) / m_baselineVoltageMv ) * 100.0 : 0.0;

    log( "AutoUV: validation passed — cap=" + std::to_string( m_mid )
         + " MHz, coreOffset=+" + std::to_string( m_bestOffset )
         + " MHz, FPS=" + std::to_string( static_cast< int >( avgFps ) )
         + " (min=" + std::to_string( static_cast< int >( minFps ) ) + ")"
         + ", power=" + std::to_string( static_cast< int >( avgPower ) )
         + " W (saved ~" + std::to_string( static_cast< int >( savedPct ) ) + "%)"
         + ", voltage=" + std::to_string( static_cast< int >( avgVoltage ) )
         + " mV (saved ~" + std::to_string( static_cast< int >( voltSavedPct ) ) + "%)" );

    enterDone( true, "Undervolt complete: cap " + std::to_string( m_mid )
               + " MHz, core offset +" + std::to_string( m_bestOffset )
               + " MHz (saved ~" + std::to_string( static_cast< int >( savedPct ) ) + "% power)" );
    break;
  }

  default:
    break;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

void AutoUndervoltWorker::resetStepMetrics()
{
  m_stepStart = std::chrono::steady_clock::now();
  if ( m_phase == UVPhase::Validating )
    m_stepDurationMs = m_config.validationMs;
  else
    m_stepDurationMs = m_config.searchTestMs;

  m_sampleCount = 0;
  m_postSettleSamples = 0;
  m_lowUtilCount = 0;
  m_peakTempC = 0;
  m_pstateSamples = 0;
  m_p0Samples = 0;
  m_droopCount = 0;
  m_powerCapCount = 0;
  m_thermalThrottleCount = 0;
  m_eventPowerCount = 0;
  m_eventThermalCount = 0;
  m_violationPowerDeltaCount = 0;
  m_violationThermalDeltaCount = 0;
  m_nvapiLimiterCount = 0;
  m_prevPowerViolationUsec.reset();
  m_prevThermalViolationUsec.reset();
  m_clockHistoryIdx = 0;
  m_clockHistoryCount = 0;
  m_slidingRefClockMHz = 0;

  m_fpsAccum = 0.0;
  m_fpsSamples = 0;
  m_stepFpsSamples.clear();
  m_powerAccum = 0.0;
  m_powerSamples = 0;
  m_voltageAccum = 0.0;
  m_voltageSamples = 0;
  m_minFpsObserved = -1.0;
}

bool AutoUndervoltWorker::evaluateStep( double &avgFpsOut,
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
                                        double &minFpsOut ) const
{
  avgFpsOut = filteredRunningAverageFps( m_stepFpsSamples,
                                         m_config.pollIntervalMs,
                                         m_config.fpsCompareWindowMs );

  minFpsOut = ( m_minFpsObserved > 0.0 ) ? m_minFpsObserved : avgFpsOut;

  validWorkloadOut = ( m_sampleCount > 0 )
    && ( m_lowUtilCount * 100 / std::max( 1, m_sampleCount ) < 50 );

  p0ResidencyPctOut = ( m_pstateSamples > 0 )
    ? ( m_p0Samples * 100 / m_pstateSamples ) : 100;

  droopPctOut = ( m_postSettleSamples > 0 )
    ? ( m_droopCount * 100 / m_postSettleSamples ) : 0;
  powerCapPctOut = ( m_postSettleSamples > 0 )
    ? ( m_powerCapCount * 100 / m_postSettleSamples ) : 0;
  thermalThrottlePctOut = ( m_postSettleSamples > 0 )
    ? ( m_thermalThrottleCount * 100 / m_postSettleSamples ) : 0;
  eventPowerPctOut = ( m_postSettleSamples > 0 )
    ? ( m_eventPowerCount * 100 / m_postSettleSamples ) : 0;
  eventThermalPctOut = ( m_postSettleSamples > 0 )
    ? ( m_eventThermalCount * 100 / m_postSettleSamples ) : 0;
  violationPowerPctOut = ( m_postSettleSamples > 0 )
    ? ( m_violationPowerDeltaCount * 100 / m_postSettleSamples ) : 0;
  violationThermalPctOut = ( m_postSettleSamples > 0 )
    ? ( m_violationThermalDeltaCount * 100 / m_postSettleSamples ) : 0;
  nvapiLimiterPctOut = ( m_postSettleSamples > 0 )
    ? ( m_nvapiLimiterCount * 100 / m_postSettleSamples ) : 0;

  const bool p0Ok = !validWorkloadOut || ( p0ResidencyPctOut >= m_config.minP0ResidencyPct );
  const bool droopOk = !validWorkloadOut || ( droopPctOut <= m_config.maxDroopPct );
  const bool pwrCapOk = !validWorkloadOut || ( powerCapPctOut <= m_config.maxPowerCapPct );
  const bool thermalOk = !validWorkloadOut || ( thermalThrottlePctOut <= m_config.maxThermalThrottlePct );
  const bool eventPowerOk = !validWorkloadOut || ( eventPowerPctOut <= m_config.maxEventPowerPct );
  const bool eventThermalOk = !validWorkloadOut || ( eventThermalPctOut <= m_config.maxEventThermalPct );
  const bool violationPowerOk = !validWorkloadOut || ( violationPowerPctOut <= m_config.maxViolationPowerPct );
  const bool violationThermalOk = !validWorkloadOut || ( violationThermalPctOut <= m_config.maxViolationThermalPct );
  const bool nvapiLimiterOk = !validWorkloadOut || ( nvapiLimiterPctOut <= m_config.maxNvapiLimiterPct );

  return p0Ok && droopOk && pwrCapOk && thermalOk
      && eventPowerOk && eventThermalOk
      && violationPowerOk && violationThermalOk
      && nvapiLimiterOk;
}

bool AutoUndervoltWorker::applyCandidate( int capMHz, int offsetMHz, UVSearchAction action )
{
  if ( capMHz <= 0 )
    return false;

  m_searchCapMHz = capMHz;
  m_searchOffsetMHz = std::max( 0, offsetMHz );
  m_lastAction = action;

  applyFreqCap( m_searchCapMHz );
  applyCoreOffset( m_searchOffsetMHz );

  m_mid = m_searchCapMHz;
  m_offsetMid = m_searchOffsetMHz;

  resetStepMetrics();
  emitProgress( "Testing cap " + std::to_string( m_searchCapMHz )
                + " MHz / offset +" + std::to_string( m_searchOffsetMHz ) + " MHz" );
  return true;
}

bool AutoUndervoltWorker::startNextSearchCandidate( UVSearchAction preferredAction )
{
  const int maxOffsetMHz = std::max( 0, m_config.maxCoreOffsetMHz );

  auto tryAction = [&]( UVSearchAction action ) -> bool
  {
    if ( action == UVSearchAction::RaiseOffset )
    {
      if ( m_blockedRaiseOffset )
        return false;
      const int nextOffset = m_stableOffsetMHz + std::max( 1, m_config.offsetStepMHz );
      if ( nextOffset > maxOffsetMHz )
      {
        m_blockedRaiseOffset = true;
        return false;
      }
      return applyCandidate( m_baselineClockMHz, nextOffset, UVSearchAction::RaiseOffset );
    }

    return false;
  };

  return tryAction( preferredAction );
}

void AutoUndervoltWorker::applyFreqCap( int capMHz )
{
  if ( capMHz <= 0 ) return;
  bool ok = m_nvml->setGpuLockedClocks( m_deviceIndex,
                                         static_cast< unsigned int >( capMHz ),
                                         static_cast< unsigned int >( capMHz ) );
  if ( !ok )
    log( "AutoUV: WARNING — failed to set GPU locked clocks to " + std::to_string( capMHz ) + " MHz" );
}

void AutoUndervoltWorker::resetFreqCap()
{
  if ( m_nvml && m_nvml->isAvailable() )
    m_nvml->resetGpuLockedClocks( m_deviceIndex );
}

void AutoUndervoltWorker::applyCoreOffset( int offsetMHz )
{
  if ( !m_nvml || !m_nvml->isAvailable() )
    return;

  (void) m_nvml->setClockOffset( m_deviceIndex,
                                 nvml::NVML_CLOCK_GRAPHICS,
                                 nvml::NVML_PSTATE_0,
                                 std::max( 0, offsetMHz ) );
}

void AutoUndervoltWorker::captureOriginalState()
{
  m_originalPowerLimitW.reset();
  m_originalGpuLockedClocks.reset();
  m_originalCoreOffsetMHz.reset();

  if ( auto pl = m_nvml->getEnforcedPowerLimitW( m_deviceIndex ); pl && *pl > 0.0 )
    m_originalPowerLimitW = *pl;

  if ( auto state = m_nvml->getOCState( m_deviceIndex ); state.has_value() )
  {
    m_originalGpuLockedClocks = state->gpuLockedClocks;
    for ( const auto &p : state->gpuPStates )
    {
      if ( p.pstate == static_cast< unsigned int >( nvml::NVML_PSTATE_0 ) )
      {
        m_originalCoreOffsetMHz = p.currentOffset;
        break;
      }
    }
  }
}

void AutoUndervoltWorker::restoreOriginalState( bool keepCurrentCap )
{
  if ( !keepCurrentCap )
  {
    if ( m_originalGpuLockedClocks.has_value() )
    {
      m_nvml->setGpuLockedClocks( m_deviceIndex,
                                  m_originalGpuLockedClocks->first,
                                  m_originalGpuLockedClocks->second );
    }
    else
    {
      m_nvml->resetGpuLockedClocks( m_deviceIndex );
    }

    const int restoreOffset = m_originalCoreOffsetMHz.value_or( 0 );
    (void) m_nvml->setClockOffset( m_deviceIndex,
                                   nvml::NVML_CLOCK_GRAPHICS,
                                   nvml::NVML_PSTATE_0,
                                   restoreOffset );
  }

  if ( m_originalPowerLimitW.has_value() && *m_originalPowerLimitW > 0.0 )
  {
    const auto milliwatts = static_cast< unsigned int >( *m_originalPowerLimitW * 1000.0 );
    m_nvml->setPowerLimit( m_deviceIndex, milliwatts );
  }
}

void AutoUndervoltWorker::ensureMaxPowerLimit()
{
  auto maxPower = m_nvml->getPowerMaxLimitW( m_deviceIndex );
  if ( maxPower && *maxPower > 0.0 )
  {
    m_maxPowerW = *maxPower;
    auto milliwatts = static_cast< unsigned int >( *maxPower * 1000.0 );
    m_nvml->setPowerLimit( m_deviceIndex, milliwatts );
    log( "AutoUV: power limit set to max (" + std::to_string( static_cast< int >( *maxPower ) ) + " W)" );
  }
}

void AutoUndervoltWorker::log( const std::string &msg )
{
  if ( m_logFn )
    m_logFn( msg );
}

void AutoUndervoltWorker::emitProgress( const std::string &msg )
{
  UndervoltProgress prog;
  prog.phase         = m_phase;
  prog.iteration     = m_iteration;
  prog.maxIterations = m_maxIterations;
  prog.currentCapMHz = ( m_phase == UVPhase::OffsetSearching ) ? m_finalCapMHz : m_mid;
  prog.bestCapMHz    = m_bestCap;
  prog.currentOffsetMHz = ( m_phase == UVPhase::OffsetSearching ) ? m_offsetMid : m_bestOffset;
  prog.bestOffsetMHz = m_bestOffset;
  prog.baselineFps   = m_baselineFps;
  prog.appName       = m_appName;
  prog.message       = msg;

  // Live metrics
  auto tempOpt  = m_nvml->getTemperatureDegC( m_deviceIndex );
  auto clkOpt   = m_nvml->getGpuClockMHz( m_deviceIndex );
  auto utilOpt  = m_nvml->getComputeUtilPct( m_deviceIndex );
  auto powerOpt = m_nvml->getPowerDrawW( m_deviceIndex );

  prog.tempC       = static_cast< int >( tempOpt.value_or( 0 ) );
  prog.gpuClockMHz = static_cast< int >( clkOpt.value_or( 0 ) );
  prog.gpuUtilPct  = static_cast< int >( utilOpt.value_or( 0 ) );
  prog.powerDrawW  = static_cast< int >( powerOpt.value_or( 0.0 ) );
  prog.fps         = ( m_fpsServer ) ? m_fpsServer->currentFps() : -1.0;

  emit progress( prog );
}
