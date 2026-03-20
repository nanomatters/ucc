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
#include "OverlayShmWriter.hpp"
#include "SysfsNode.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <syslog.h>

namespace
{
constexpr const char *AUTO_UV_CHECKPOINT_PATH = "/etc/ucc/autouv_checkpoint.json";

std::string toLowerCopy( std::string s )
{
  std::transform( s.begin(), s.end(), s.begin(),
                  []( unsigned char c ) { return static_cast< char >( std::tolower( c ) ); } );
  return s;
}

bool containsAnyToken( const std::string &text,
                       const std::initializer_list< const char * > tokens )
{
  for ( const char *token : tokens )
  {
    if ( text.find( token ) != std::string::npos )
      return true;
  }
  return false;
}

bool isLikelyLaptopChassis()
{
  // SMBIOS chassis-type values often used for portable systems.
  // 8: Portable, 9: Laptop, 10: Notebook, 14: Sub Notebook.
  auto chassisTypeStr = SysfsNode< std::string >( "/sys/class/dmi/id/chassis_type" ).read();
  if ( !chassisTypeStr.has_value() )
    return false;

  try
  {
    const int t = std::stoi( *chassisTypeStr );
    return t == 8 || t == 9 || t == 10 || t == 14;
  }
  catch ( ... )
  {
  }

  return false;
}

bool isLikelyDesktopNvidiaGpu( const NvmlOCState &ocState )
{
  const std::string name = toLowerCopy( ocState.gpuName );

  // Strong mobile markers in NVIDIA marketing names.
  if ( containsAnyToken( name, { "laptop", "max-q", "max q", "mobile", "notebook" } ) )
    return false;

  // Power envelope is a strong discriminator for mobile SKUs.
  // Treat <=130W as likely mobile, >=170W as likely desktop-class.
  const double pmax = ocState.powerMaxW;
  if ( pmax > 0.0 && pmax <= 130.0 )
    return false;
  if ( pmax >= 170.0 )
    return true;

  // If the host chassis is clearly laptop/subnotebook, bias toward mobile
  // unless power envelope already indicates desktop-class above.
  if ( isLikelyLaptopChassis() )
    return false;

  // Fallback: assume desktop for untagged names on non-laptop chassis.
  return true;
}

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

double runningAverageFps( const std::vector< double > &samples,
                          int pollIntervalMs,
                          int windowMs )
{
  if ( samples.empty() )
    return -1.0;

  const int safePollMs = std::max( 1, pollIntervalMs );
  const int safeWindowMs = std::max( safePollMs, windowMs );
  const size_t windowSamples = static_cast< size_t >( std::max( 1, safeWindowMs / safePollMs ) );

  const size_t start = ( samples.size() > windowSamples )
    ? ( samples.size() - windowSamples )
    : 0;

  double sum = 0.0;
  size_t count = 0;
  for ( size_t i = start; i < samples.size(); ++i )
  {
    sum += samples[i];
    ++count;
  }

  return ( count > 0 ) ? ( sum / static_cast< double >( count ) ) : -1.0;
}

/// Compute standard deviation and 1% low from an FPS sample window (#3).
struct FpsStats
{
  double mean    = 0.0;
  double stddev  = 0.0;
  double pct1Low = 0.0; ///< 1st-percentile (1% low)
};

FpsStats computeFpsStats( const std::vector< double > &samples,
                          int pollIntervalMs, int windowMs )
{
  FpsStats out{};
  if ( samples.empty() )
    return out;

  const int safePollMs = std::max( 1, pollIntervalMs );
  const int safeWindowMs = std::max( safePollMs, windowMs );
  const size_t windowSamples = static_cast< size_t >( std::max( 1, safeWindowMs / safePollMs ) );
  const size_t start = ( samples.size() > windowSamples )
    ? ( samples.size() - windowSamples ) : 0;
  const size_t n = samples.size() - start;
  if ( n == 0 )
    return out;

  // Mean
  double sum = 0.0;
  for ( size_t i = start; i < samples.size(); ++i )
    sum += samples[i];
  out.mean = sum / static_cast< double >( n );

  // Stddev
  double sqSum = 0.0;
  for ( size_t i = start; i < samples.size(); ++i )
  {
    const double d = samples[i] - out.mean;
    sqSum += d * d;
  }
  out.stddev = std::sqrt( sqSum / static_cast< double >( n ) );

  // 1% low: sort tail, take 1st-percentile value
  std::vector< double > sorted;
  sorted.reserve( n );
  for ( size_t i = start; i < samples.size(); ++i )
    sorted.push_back( samples[i] );
  std::sort( sorted.begin(), sorted.end() );
  const size_t idx = std::max< size_t >( 0, static_cast< size_t >( static_cast< double >( n ) * 0.01 ) );
  out.pct1Low = sorted[idx];

  return out;
}

const char *uvPhaseToString( UVPhase phase )
{
  switch ( phase )
  {
  case UVPhase::Idle: return "idle";
  case UVPhase::Baseline: return "baseline";
  case UVPhase::CapReduction: return "cap_reduction";
  case UVPhase::Searching: return "searching";
  case UVPhase::OffsetSearching: return "offset_searching";
  case UVPhase::Validating: return "validating";
  case UVPhase::PowerLimitSweep: return "power_limit_sweep";
  case UVPhase::Done: return "done";
  }
  return "idle";
}

UVPhase uvPhaseFromString( const std::string &value )
{
  if ( value == "baseline" ) return UVPhase::Baseline;
  if ( value == "cap_reduction" ) return UVPhase::CapReduction;
  if ( value == "searching" ) return UVPhase::Searching;
  if ( value == "offset_searching" ) return UVPhase::OffsetSearching;
  if ( value == "validating" ) return UVPhase::Validating;
  if ( value == "power_limit_sweep" ) return UVPhase::PowerLimitSweep;
  if ( value == "done" ) return UVPhase::Done;
  return UVPhase::Idle;
}

const char *searchActionToString( UVSearchAction action )
{
  switch ( action )
  {
  case UVSearchAction::None: return "none";
  case UVSearchAction::LowerCap: return "lower_cap";
  case UVSearchAction::RaiseOffset: return "raise_offset";
  }
  return "none";
}

UVSearchAction searchActionFromString( const std::string &value )
{
  if ( value == "lower_cap" ) return UVSearchAction::LowerCap;
  if ( value == "raise_offset" ) return UVSearchAction::RaiseOffset;
  return UVSearchAction::None;
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

  if ( tryResumeFromCheckpoint( deviceIndex ) )
    return true;

  // Platform-adaptive offset search defaults:
  // desktop GPUs get a larger sweep (700 MHz max, 35 MHz steps), while
  // mobile-tagged GPUs keep their configured values.
  if ( auto ocState = m_nvml->getOCState( deviceIndex ); ocState.has_value() )
  {
    if ( isLikelyDesktopNvidiaGpu( *ocState ) )
    {
      m_config.maxCoreOffsetMHz = 700;
      m_config.offsetStepMHz = 35;
      log( "AutoUV: desktop GPU detected ('" + ocState->gpuName
           + "') — using offset sweep max=700 MHz, step=35 MHz" );
    }
    else
    {
      log( "AutoUV: mobile GPU detected ('" + ocState->gpuName
           + "') — using offset sweep max="
           + std::to_string( m_config.maxCoreOffsetMHz ) + " MHz, step="
           + std::to_string( m_config.offsetStepMHz ) + " MHz" );
    }
  }

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

void AutoUndervoltWorker::tickBackgroundMonitor( const std::string &appName,
                                                  unsigned int deviceIndex )
{
  // Only monitor when no scan is running and a profile is active.
  if ( m_phase != UVPhase::Idle && m_phase != UVPhase::Done )
    return;

  if ( appName.empty() )
    return;

  auto it = m_appProfiles.find( appName );
  if ( it == m_appProfiles.end() )
    return;

  // Rate-limit checks to once every 5 seconds.
  const auto now = std::chrono::steady_clock::now();
  if ( m_bgMonApp == appName && m_bgMonLastCheck.time_since_epoch().count() != 0 )
  {
    const auto elapsedMs = std::chrono::duration_cast< std::chrono::milliseconds >(
      now - m_bgMonLastCheck ).count();
    if ( elapsedMs < 5000 )
      return;
  }

  // Reset counters when app changes.
  if ( m_bgMonApp != appName )
  {
    m_bgMonApp = appName;
    m_bgMonDroopCount = 0;
    m_bgMonSampleCount = 0;
  }
  m_bgMonLastCheck = now;

  // Check if actual clock is significantly below the locked cap, indicating instability.
  auto clkOpt = m_nvml->getGpuClockMHz( deviceIndex );
  if ( !clkOpt )
    return;

  const int actualClk = static_cast< int >( *clkOpt );
  const int profileCap = it->second.gpuFreqCapMHz;
  const int droopThreshold = static_cast< int >( profileCap * 0.90 ); // 10% droop

  ++m_bgMonSampleCount;
  if ( actualClk < droopThreshold )
    ++m_bgMonDroopCount;

  // After 12 samples (~60 seconds at 5s interval), evaluate.
  constexpr int kWindowSamples = 12;
  if ( m_bgMonSampleCount >= kWindowSamples )
  {
    const int droopPct = ( m_bgMonDroopCount * 100 ) / m_bgMonSampleCount;
    if ( droopPct > 50 )
    {
      // Sustained instability: reduce offset by half and reapply.
      const int oldOffset = it->second.coreOffsetMHz;
      const int newOffset = std::max( 0, oldOffset / 2 );
      it->second.coreOffsetMHz = newOffset;
      it->second.lastUsed = std::chrono::system_clock::now();

      (void) m_nvml->setClockOffset( deviceIndex, nvml::NVML_CLOCK_GRAPHICS,
                                       nvml::NVML_PSTATE_0, newOffset );

      log( "AutoUV: background monitor — sustained clock droop for '" + appName
           + "' (" + std::to_string( droopPct ) + "% drooped), reduced offset from +"
           + std::to_string( oldOffset ) + " to +" + std::to_string( newOffset ) + " MHz" );
    }

    // Reset window.
    m_bgMonDroopCount = 0;
    m_bgMonSampleCount = 0;
  }
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
  // Include thermal soak time before the real baseline measurement (#6).
  m_stepDurationMs = m_config.thermalSoakMs + m_config.baselineMs;
  m_sampleCount    = 0;
  m_lowUtilCount   = 0;
  m_peakTempC      = 0;
  m_pstateSamples  = 0;
  m_p0Samples      = 0;

  m_baselineClockMHz = 0;
  m_baselineMaxClockMHz = 0;
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
  m_soakTempSamples.clear();

  // Reset crash detection state
  m_noFpsTicks  = 0;
  m_hadFpsData  = false;

  // Reset validation oscillation state (#8)
  m_validationCapDir = 0;
  m_validationOscillations = 0;

  log( "AutoUV: Phase 0 — Thermal soak ("
       + std::to_string( m_config.thermalSoakMs / 1000 )
       + " s) + Baseline (" + std::to_string( m_config.baselineMs / 1000 ) + " s)" );
  emitProgress( "Thermal soak — waiting for GPU temperature to stabilise..." );
  saveCheckpoint( true );

  m_pollTimer->start( m_config.pollIntervalMs );
}

void AutoUndervoltWorker::enterCapReduction()
{
  m_phase = UVPhase::CapReduction;
  // In power-limit mode, keep the reduced power limit instead of
  // raising to max — the PL sweep already found the best wattage.
  if ( !m_config.powerLimitMode )
    ensureMaxPowerLimit();

  // Start at the peak observed clock from baseline and step down.
  m_capReductionCurrentMHz = m_baselineMaxClockMHz;
  m_capReductionPrevMHz    = m_baselineMaxClockMHz;
  m_capReductionFpsAccum   = 0.0;
  m_capReductionFpsSamples = 0;
  m_capReductionFineMode   = false;

  // First step: reduce by one increment from baseline.
  m_capReductionCurrentMHz -= m_config.capReductionStepMHz;
  if ( m_capReductionCurrentMHz < m_config.capReductionStepMHz )
  {
    // Cannot reduce below a meaningful minimum — skip directly to offset search.
    log( "AutoUV: CapReduction skipped — baseline clock too low to reduce" );
    enterSearch();
    return;
  }

  applyFreqCap( m_capReductionCurrentMHz );
  applyCoreOffset( 0 );

  resetStepMetrics();

  log( "AutoUV: Phase 0b — Cap-reduction toward target FPS "
       + std::to_string( static_cast< int >( m_config.targetFpsValue ) )
       + ", starting cap=" + std::to_string( m_capReductionCurrentMHz ) + " MHz" );
  emitProgress( "Reducing cap toward target FPS..." );
  saveCheckpoint( true );
}

void AutoUndervoltWorker::enterSearch()
{
  m_phase = UVPhase::Searching;
  if ( !m_config.powerLimitMode )
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
  m_prevSearchAvgFps = m_baselineFps;

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

  saveCheckpoint( true );
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

  // Use extended validation duration if configured (#7).
  const int valMs = m_config.extendedValidation
    ? m_config.extendedValidationMs : m_config.validationMs;

  // Reset oscillation guard (#8).
  m_validationCapDir = 0;
  m_validationOscillations = 0;

  log( "AutoUV: Phase 3 — Validation at cap=" + std::to_string( m_mid )
       + " MHz, coreOffset=+" + std::to_string( m_bestOffset )
       + " MHz (" + std::to_string( valMs / 1000 ) + " s"
       + ( m_config.extendedValidation ? ", extended" : "" ) + ")" );

  applyFreqCap( m_mid );
  applyCoreOffset( m_bestOffset );

  resetStepMetrics();
  m_stepDurationMs = valMs;

  emitProgress( "Validating cap " + std::to_string( m_mid ) + " MHz / offset +"
                + std::to_string( m_bestOffset ) + " MHz ("
                + std::to_string( valMs / 1000 ) + " s)" );
  saveCheckpoint( true );
}

void AutoUndervoltWorker::enterPowerLimitSweep()
{
  m_phase = UVPhase::PowerLimitSweep;

  // Read hardware power limit constraints.
  auto defaultW = m_nvml->getPowerDefaultLimitW( m_deviceIndex );
  auto minW     = m_nvml->getPowerMinLimitW( m_deviceIndex );
  auto maxW     = m_nvml->getPowerMaxLimitW( m_deviceIndex );

  m_plDefaultMW = defaultW ? static_cast< unsigned int >( *defaultW * 1000.0 ) : 0;
  m_plMinMW     = minW     ? static_cast< unsigned int >( *minW * 1000.0 )     : 0;
  const unsigned int maxMW = maxW ? static_cast< unsigned int >( *maxW * 1000.0 ) : 0;

  if ( m_plDefaultMW == 0 || m_plMinMW == 0 )
  {
    enterDone( false, "Power-limit mode: could not read power limit constraints" );
    return;
  }

  // Start at default power limit (not max — max might be above spec).
  m_plCurrentMW = m_plDefaultMW;
  m_plBestMW = m_plDefaultMW;
  m_plBestFps = m_baselineFps;
  m_plBaselineFps = m_baselineFps;

  const int stepMW = m_config.powerLimitStepW * 1000;
  const int rangeMW = static_cast< int >( m_plDefaultMW - m_plMinMW );
  m_maxIterations = ( stepMW > 0 ) ? ( rangeMW / stepMW + 1 ) : 1;
  m_iteration = 0;

  log( "AutoUV: Phase PL — Power-limit sweep from "
       + std::to_string( m_plDefaultMW / 1000 ) + " W down to min "
       + std::to_string( m_plMinMW / 1000 ) + " W (step="
       + std::to_string( m_config.powerLimitStepW ) + " W, max "
       + std::to_string( maxMW / 1000 ) + " W)" );

  // Set to default first and measure.
  m_nvml->setPowerLimit( m_deviceIndex, m_plCurrentMW );
  resetStepMetrics();
  m_stepDurationMs = m_config.powerLimitTestMs;

  emitProgress( "Power-limit sweep: " + std::to_string( m_plCurrentMW / 1000 ) + " W" );
  saveCheckpoint( true );
}

// ─────────────────────────────────────────────────────────────────────────────
// Crash suspend — save state and stop without clearing the checkpoint so
// the scan can be resumed when the application is relaunched.
// ─────────────────────────────────────────────────────────────────────────────

void AutoUndervoltWorker::enterCrashSuspend( const std::string &msg )
{
  m_pollTimer->stop();

  log( "AutoUV: crash detected — " + msg );
  saveCheckpoint( true, "crash_detected" );  // persist current state for later resume

  // Restore GPU to safe defaults but don't clear the checkpoint.
  restoreOriginalState( false );

  UndervoltResult result;
  result.gpuFreqCapMHz  = 0;
  result.coreOffsetMHz  = 0;
  result.baselineClkMHz = m_baselineClockMHz;
  result.baselineFps    = m_baselineFps;
  result.finalFps       = 0.0;
  result.finalPowerW    = 0.0;
  result.powerSavedPct  = 0.0;
  result.success        = false;
  result.appName        = m_appName;
  result.message        = "Suspended: " + msg
    + ". Progress has been saved — restart the application and resume the scan.";

  m_phase = UVPhase::Done;

  emitProgress( result.message );
  OverlayShmWriter::instance().setInactive();
  emit finished( result );
}

void AutoUndervoltWorker::enterDone( bool success, const std::string &msg )
{
  m_pollTimer->stop();
  m_phase = UVPhase::Done;
  clearCheckpoint();

  restoreOriginalState( success );

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

  // FPS/W efficiency metric
  double finalFpsPerWatt = 0.0;
  double efficiencyGainPct = 0.0;
  if ( success && finalAvgPower > 0.0 && finalFps > 0.0 )
  {
    finalFpsPerWatt = finalFps / finalAvgPower;
    if ( m_baselineFpsPerWatt > 0.0 )
      efficiencyGainPct = ( ( finalFpsPerWatt - m_baselineFpsPerWatt ) / m_baselineFpsPerWatt ) * 100.0;
    log( "AutoUV: efficiency — " + std::to_string( finalFpsPerWatt )
         + " FPS/W (baseline " + std::to_string( m_baselineFpsPerWatt )
         + " FPS/W, gain " + std::to_string( static_cast< int >( efficiencyGainPct ) ) + "%)" );
  }

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
  result.fpsPerWatt        = finalFpsPerWatt;
  result.baselineFpsPerWatt = m_baselineFpsPerWatt;
  result.efficiencyGainPct = efficiencyGainPct;
  result.finalPowerLimitW = m_config.powerLimitMode
    ? static_cast< int >( m_plBestMW / 1000 ) : 0;
  result.success        = success;
  result.appName        = m_appName;
  result.message        = msg;

  emitProgress( msg );
  OverlayShmWriter::instance().setInactive();
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

      m_hadFpsData = true;
      m_noFpsTicks = 0;
    }
    else if ( m_hadFpsData )
    {
      // FPS was flowing before but stopped — count consecutive misses.
      ++m_noFpsTicks;
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

  // ── Crash detection: FPS source disappeared ──
  if ( m_hadFpsData && m_noFpsTicks >= kCrashNoFpsTicks )
  {
    const bool processGone = m_fpsServer && m_fpsServer->clientPid() == 0;
    std::string reason = processGone
      ? "Application process disappeared (likely crashed)"
      : "FPS data stopped for " + std::to_string( m_noFpsTicks * m_config.pollIntervalMs / 1000 )
        + " seconds (application may have crashed or exited)";
    enterCrashSuspend( reason );
    return;
  }

  // ── Phase dispatch ──
  switch ( m_phase )
  {
  // ────────────────────────────────────────────────────────────────────────
  case UVPhase::Baseline:
  {
    const bool inSoakPhase = ( elapsedMs < m_config.thermalSoakMs );

    // Collect thermal soak temperature samples (#6).
    if ( inSoakPhase && tempC > 0 )
      m_soakTempSamples.push_back( tempC );

    // During soak, skip early to stable baseline if temperature has settled.
    if ( inSoakPhase && elapsedMs > 5000 && isThermallyStable() )
    {
      log( "AutoUV: thermal soak — GPU temperature stable early, proceeding to baseline" );
      // Reset data so only post-soak samples are used for baseline.
      m_baselineFpsSamples.clear();
      m_fpsAccum = 0.0;
      m_fpsSamples = 0;
      m_powerAccum = 0.0;
      m_powerSamples = 0;
      m_voltageAccum = 0.0;
      m_voltageSamples = 0;
      m_clkSampleCount = 0;
      m_sampleCount = 0;
      m_lowUtilCount = 0;
      // Fast-forward m_stepStart so elapsed time >= thermalSoakMs on the
      // next tick — this prevents inSoakPhase from becoming true again.
      m_stepStart = std::chrono::steady_clock::now()
                    - std::chrono::milliseconds( m_config.thermalSoakMs );
      m_soakTempSamples.clear();  // mark soak as done
      emitProgress( "Measuring baseline (thermal soak done)..." );
      break;
    }

    if ( inSoakPhase )
    {
      emitProgress( "Thermal soak: " + std::to_string( elapsedMs / 1000 ) + "/"
                    + std::to_string( m_config.thermalSoakMs / 1000 )
                    + " s (" + std::to_string( tempC ) + " °C)" );
      break;
    }

    // If we just crossed from soak into baseline, reset data once.
    if ( !m_soakTempSamples.empty()
         && m_baselineFpsSamples.empty() && m_clkSampleCount == 0 )
    {
      log( "AutoUV: thermal soak complete — measuring baseline" );
      m_fpsAccum = 0.0;
      m_fpsSamples = 0;
      m_powerAccum = 0.0;
      m_powerSamples = 0;
      m_voltageAccum = 0.0;
      m_voltageSamples = 0;
      m_sampleCount = 0;
      m_lowUtilCount = 0;
      emitProgress( "Measuring baseline (keep workload running)..." );
    }

    // Accumulate clock samples during the real baseline portion.
    if ( gpuClk > 0 && m_clkSampleCount < MAX_CLK_SAMPLES )
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
        m_baselineMaxClockMHz = m_clkSamples[ m_clkSampleCount - 1 ];
      }
      else if ( gpuClk > 0 )
      {
        m_baselineClockMHz = gpuClk;
        m_baselineMaxClockMHz = gpuClk;
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

      // Compute baseline efficiency — FPS per watt (#11)
      m_baselineFpsPerWatt = ( m_baselinePowerW > 0.0 )
        ? m_baselineFps / m_baselinePowerW : 0.0;

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
           + " mV, FPS/W=" + std::to_string( m_baselineFpsPerWatt ).substr( 0, 5 )
           + ", temp=" + std::to_string( m_baselineTempC ) + " °C"
           + ", app='" + m_appName + "'" );

      // If the user set a target FPS below baseline, first reduce the freq cap
      // in large steps before starting the fine offset sweep.
      if ( m_config.powerLimitMode )
      {
        enterPowerLimitSweep();
      }
      else if ( m_config.targetFpsEnabled
           && m_config.targetFpsValue > 0.0
           && m_config.targetFpsValue < m_baselineFps )
      {
        enterCapReduction();
      }
      else
      {
        enterSearch();
      }
    }
    else
    {
      emitProgress( "Baseline: " + std::to_string( elapsedMs / 1000 ) + "/"
                    + std::to_string( m_stepDurationMs / 1000 ) + " s" );
    }
    break;
  }

  // ────────────────────────────────────────────────────────────────────────
  case UVPhase::CapReduction:
  {
    if ( !stepComplete )
    {
      emitProgress( "Cap-reduction: cap=" + std::to_string( m_capReductionCurrentMHz )
                    + " MHz (" + std::to_string( elapsedMs / 1000 ) + "/"
                    + std::to_string( m_stepDurationMs / 1000 ) + " s)" );
      break;
    }

    // Use strict arithmetic period average for cap-reduction decisions.
    const double windowAvgFps = runningAverageFps(
      m_stepFpsSamples, m_config.pollIntervalMs, m_config.capReductionWindowMs );

    log( "AutoUV: CapReduction — cap=" + std::to_string( m_capReductionCurrentMHz )
         + " MHz, avgFPS=" + std::to_string( static_cast< int >( windowAvgFps ) )
         + ", target=" + std::to_string( static_cast< int >( m_config.targetFpsValue ) ) );

    if ( windowAvgFps <= m_config.targetFpsValue )
    {
      // If we're in fine mode, the step was small — use current cap as the
      // result (we're close enough to target without overshoot).
      if ( m_capReductionFineMode )
      {
        m_baselineClockMHz = m_capReductionCurrentMHz;
        log( "AutoUV: CapReduction — target reached in fine mode, using cap="
             + std::to_string( m_baselineClockMHz ) + " MHz for offset sweep" );
      }
      else
      {
        // Coarse step overshot — switch to fine mode, revert to previous cap
        // and step down with fine granularity.
        m_capReductionFineMode = true;
        m_capReductionCurrentMHz = m_capReductionPrevMHz;
        m_capReductionCurrentMHz -= m_config.capReductionFineStepMHz;
        if ( m_capReductionCurrentMHz < m_config.capReductionFineStepMHz )
        {
          m_baselineClockMHz = m_capReductionPrevMHz;
          log( "AutoUV: CapReduction — overshot at minimum fine step, using cap="
               + std::to_string( m_baselineClockMHz ) + " MHz" );
        }
        else
        {
          applyFreqCap( m_capReductionCurrentMHz );
          resetStepMetrics();
          log( "AutoUV: CapReduction — switching to fine mode ("
               + std::to_string( m_config.capReductionFineStepMHz )
               + " MHz steps), cap=" + std::to_string( m_capReductionCurrentMHz ) + " MHz" );
          break;
        }
      }
      enterSearch();
      break;
    }

    // FPS still at or above target — record this cap and step down further.
    m_capReductionPrevMHz = m_capReductionCurrentMHz;
    const int step = m_capReductionFineMode
      ? m_config.capReductionFineStepMHz
      : m_config.capReductionStepMHz;
    m_capReductionCurrentMHz -= step;

    // Approaching target? Switch to fine mode proactively (#5).
    // If FPS is within 20% of the gap between current and target, switch.
    if ( !m_capReductionFineMode && windowAvgFps > 0.0 && m_config.targetFpsValue > 0.0 )
    {
      const double ratio = windowAvgFps / m_config.targetFpsValue;
      if ( ratio < 1.3 ) // FPS within 30% of target — use fine steps
      {
        m_capReductionFineMode = true;
        log( "AutoUV: CapReduction — FPS approaching target, switching to fine steps" );
      }
    }

    const int minCap = std::max( m_config.capReductionFineStepMHz, 200 );
    if ( m_capReductionCurrentMHz < minCap )
    {
      // Hit minimum — use current as the cap.
      m_baselineClockMHz = m_capReductionPrevMHz;
      log( "AutoUV: CapReduction — hit minimum cap, using cap="
           + std::to_string( m_baselineClockMHz ) + " MHz for offset sweep" );
      enterSearch();
      break;
    }

    // Next step: apply new lower cap and measure again.
    applyFreqCap( m_capReductionCurrentMHz );
    resetStepMetrics();
    emitProgress( "Cap-reduction: cap=" + std::to_string( m_capReductionCurrentMHz ) + " MHz" );
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

    // Frame time stutter gate (#3): compute FPS variance for this step.
    const auto fpsStats = computeFpsStats( m_stepFpsSamples,
      m_config.pollIntervalMs, m_config.fpsCompareWindowMs );
    const double fpsStddevPct = ( fpsStats.mean > 0.0 )
      ? ( fpsStats.stddev / fpsStats.mean ) * 100.0 : 0.0;
    const double pct1LowRatio = ( fpsStats.mean > 0.0 )
      ? fpsStats.pct1Low / fpsStats.mean : 1.0;
    const bool stutterDetected = validWorkload
      && ( fpsStddevPct > m_config.maxFpsStddevPct
           || pct1LowRatio < m_config.min1PctLowRatio );

    // Voltage sanity check (#4): if offset pushes voltage *above* baseline,
    // the V/F shift is counterproductive — stop searching further.
    bool voltageRose = false;
    if ( m_baselineVoltageMv > 0.0 && m_voltageSamples > 0 && m_searchOffsetMHz > 0 )
    {
      const double curVoltage = m_voltageAccum / static_cast< double >( m_voltageSamples );
      const double risePct = ( ( curVoltage - m_baselineVoltageMv ) / m_baselineVoltageMv ) * 100.0;
      if ( risePct > m_config.maxVoltageRisePct )
      {
        voltageRose = true;
        log( "AutoUV: voltage sanity failed at offset +" + std::to_string( m_searchOffsetMHz )
             + " MHz — voltage " + std::to_string( static_cast< int >( curVoltage ) )
             + " mV is " + std::to_string( static_cast< int >( risePct ) )
             + "% above baseline " + std::to_string( static_cast< int >( m_baselineVoltageMv ) )
             + " mV — stopping offset sweep" );
      }
    }

    // P-state residency is noisy/architecture-dependent when clocks are locked.
    // Keep it as telemetry but gate offset stability primarily on clock droop.
    const bool stepOk = !validWorkload
      || ( droopPct <= m_config.maxDroopPct );
    const bool fpsRegressed = ( m_searchOffsetMHz > m_stableOffsetMHz )
      && ( m_prevSearchAvgFps > 0.0 )
      && ( avgFps > 0.0 )
      && ( avgFps < ( m_prevSearchAvgFps * ( 1.0 - m_config.fpsDropPct / 100.0 ) ) );

        log( "AutoUV: offset=+" + std::to_string( m_searchOffsetMHz )
         + " MHz — P0=" + std::to_string( p0ResidencyPct )
         + "%, droop=" + std::to_string( droopPct )
          + "%, avgFPS=" + std::to_string( static_cast< int >( avgFps ) )
          + " vs prev=" + std::to_string( static_cast< int >( m_prevSearchAvgFps ) )
         + "%, stddev=" + std::to_string( static_cast< int >( fpsStddevPct ) )
         + "%, 1%low=" + std::to_string( static_cast< int >( fpsStats.pct1Low ) )
         + "% [gates], pwrCap=" + std::to_string( powerCapPct )
         + "%, thermal=" + std::to_string( thermalThrottlePct )
         + "%, evtPwr=" + std::to_string( eventPowerPct )
         + "%, evtThm=" + std::to_string( eventThermalPct )
         + "%, vioPwr=" + std::to_string( violationPowerPct )
         + "%, vioThm=" + std::to_string( violationThermalPct )
         + "%, nvapiLim=" + std::to_string( nvapiLimiterPct )
          + ( voltageRose ? "%, VOLTAGE_ROSE" : "%" )
          + ( stutterDetected ? ", STUTTER" : "" )
          + " [info] => " + ( ( stepOk && !fpsRegressed && !stutterDetected && !voltageRose ) ? "STABLE" : "UNSTABLE" ) );

    // Voltage rise means no further offset increases can help (#4).
    if ( voltageRose )
    {
      m_blockedRaiseOffset = true;
      m_searchCapMHz = m_baselineClockMHz;
      m_searchOffsetMHz = m_stableOffsetMHz;
      m_bestCap = m_stableCapMHz;
      m_bestOffset = m_stableOffsetMHz;
      applyFreqCap( m_searchCapMHz );
      applyCoreOffset( m_searchOffsetMHz );
      enterValidation();
      break;
    }

    if ( fpsRegressed )
    {
      log( "AutoUV: stopping offset sweep at +" + std::to_string( m_searchOffsetMHz )
           + " MHz — avg FPS " + std::to_string( static_cast< int >( avgFps ) )
           + " fell more than " + std::to_string( m_config.fpsDropPct )
           + "% below previous period "
           + std::to_string( static_cast< int >( m_prevSearchAvgFps ) )
           + "; using previous offset +" + std::to_string( m_stableOffsetMHz ) + " MHz" );

      m_blockedRaiseOffset = true;
      m_searchCapMHz = m_baselineClockMHz;
      m_searchOffsetMHz = m_stableOffsetMHz;
      m_bestCap = m_stableCapMHz;
      m_bestOffset = m_stableOffsetMHz;
      applyFreqCap( m_searchCapMHz );
      applyCoreOffset( m_searchOffsetMHz );
      enterValidation();
      break;
    }

    if ( !stepOk )
    {
      log( "AutoUV: unstable reason at offset +" + std::to_string( m_searchOffsetMHz )
           + " MHz: droop=" + std::to_string( droopPct )
           + "% > maxDroopPct=" + std::to_string( m_config.maxDroopPct )
           + "% (P0=" + std::to_string( p0ResidencyPct ) + "%, informational)" );
    }

    if ( stutterDetected )
    {
      log( "AutoUV: stutter detected at offset +" + std::to_string( m_searchOffsetMHz )
           + " MHz — stddev=" + std::to_string( static_cast< int >( fpsStddevPct ) )
           + "% (max " + std::to_string( static_cast< int >( m_config.maxFpsStddevPct ) )
           + "%), 1%low=" + std::to_string( static_cast< int >( fpsStats.pct1Low ) )
           + " (min ratio " + std::to_string( m_config.min1PctLowRatio ).substr( 0, 4 )
           + ") — reverting to stable offset +" + std::to_string( m_stableOffsetMHz ) + " MHz" );

      m_blockedRaiseOffset = true;
      m_searchCapMHz = m_baselineClockMHz;
      m_searchOffsetMHz = m_stableOffsetMHz;
      m_bestCap = m_stableCapMHz;
      m_bestOffset = m_stableOffsetMHz;
      applyFreqCap( m_searchCapMHz );
      applyCoreOffset( m_searchOffsetMHz );
      enterValidation();
      break;
    }

    if ( stepOk )
    {
      m_stableCapMHz = m_baselineClockMHz;
      m_stableOffsetMHz = m_searchOffsetMHz;
      m_bestCap = m_stableCapMHz;
      m_bestOffset = m_stableOffsetMHz;
      m_prevSearchAvgFps = avgFps;

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

    // Oscillation guard: if cap has bounced back and forth too many times,
    // freeze at the current cap and accept the result.
    const bool oscillationFrozen =
      m_validationOscillations >= m_config.maxValidationOscillations;

    if ( !oscillationFrozen && avgFps > highBand && ( m_mid - capStep ) >= minCapMHz )
    {
      // Track direction: -1 = lowering
      if ( m_validationCapDir == 1 )
        ++m_validationOscillations;
      m_validationCapDir = -1;

      if ( m_validationOscillations >= m_config.maxValidationOscillations )
      {
        log( "AutoUV: validation oscillation limit reached ("
             + std::to_string( m_validationOscillations )
             + " reversals) — freezing cap at " + std::to_string( m_mid ) + " MHz" );
        // Fall through to accept result below
      }
      else
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
    }

    if ( !oscillationFrozen && avgFps < lowBand && ( m_mid + capStep ) <= m_baselineClockMHz )
    {
      // Track direction: +1 = raising
      if ( m_validationCapDir == -1 )
        ++m_validationOscillations;
      m_validationCapDir = 1;

      if ( m_validationOscillations >= m_config.maxValidationOscillations )
      {
        log( "AutoUV: validation oscillation limit reached ("
             + std::to_string( m_validationOscillations )
             + " reversals) — freezing cap at " + std::to_string( m_mid ) + " MHz" );
        // Fall through to accept result below
      }
      else
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

  // ────────────────────────────────────────────────────────────────────────
  case UVPhase::PowerLimitSweep:
  {
    if ( !stepComplete )
    {
      emitProgress( "Power-limit sweep: " + std::to_string( m_plCurrentMW / 1000 )
                    + " W (" + std::to_string( elapsedMs / 1000 ) + "/"
                    + std::to_string( m_stepDurationMs / 1000 ) + " s)" );
      break;
    }

    double avgFps = 0.0;
    bool validWorkload = false;
    int p0ResidencyPct = 0, droopPct = 0, powerCapPct = 0, thermalThrottlePct = 0;
    int eventPowerPct = 0, eventThermalPct = 0;
    int violationPowerPct = 0, violationThermalPct = 0;
    int nvapiLimiterPct = 0;
    double minFps = 0.0;

    (void) evaluateStep( avgFps, validWorkload, p0ResidencyPct,
                         droopPct, powerCapPct, thermalThrottlePct,
                         eventPowerPct, eventThermalPct,
                         violationPowerPct, violationThermalPct,
                         nvapiLimiterPct, minFps );

    ++m_iteration;

    const double targetFps = std::max( 1.0, m_targetFps );
    const double lowBand = targetFps * ( 1.0 - m_config.fpsDropPct / 100.0 );
    const bool fpsOk = !validWorkload || ( avgFps >= lowBand );

    log( "AutoUV: PL step — " + std::to_string( m_plCurrentMW / 1000 )
         + " W, FPS=" + std::to_string( static_cast< int >( avgFps ) )
         + " (target=" + std::to_string( static_cast< int >( targetFps ) )
         + ", thres=" + std::to_string( static_cast< int >( lowBand ) )
         + "), valid=" + std::to_string( validWorkload )
         + ", ok=" + std::to_string( fpsOk ) );

    if ( fpsOk )
    {
      // This power limit is stable — record as best.
      m_plBestMW = m_plCurrentMW;
      m_plBestFps = avgFps;

      // Try to reduce further.
      const unsigned int stepMW = static_cast< unsigned int >( m_config.powerLimitStepW * 1000 );
      if ( m_plCurrentMW > m_plMinMW + stepMW )
      {
        m_plCurrentMW -= stepMW;
        if ( m_plCurrentMW < m_plMinMW )
          m_plCurrentMW = m_plMinMW;

        m_nvml->setPowerLimit( m_deviceIndex, m_plCurrentMW );
        resetStepMetrics();
        m_stepDurationMs = m_config.powerLimitTestMs;

        emitProgress( "Power-limit sweep: " + std::to_string( m_plCurrentMW / 1000 ) + " W" );
        break;
      }
      else
      {
        // At minimum — accept.
        m_plBestMW = m_plCurrentMW;
        m_plBestFps = avgFps;
      }
    }

    // Either FPS dropped or we reached the minimum. Use best power limit.
    const double savedPct = ( m_plDefaultMW > 0 )
      ? ( static_cast< double >( m_plDefaultMW - m_plBestMW ) / m_plDefaultMW ) * 100.0 : 0.0;

    log( "AutoUV: power-limit sweep done — best=" + std::to_string( m_plBestMW / 1000 )
         + " W (default=" + std::to_string( m_plDefaultMW / 1000 )
         + " W, saved ~" + std::to_string( static_cast< int >( savedPct ) ) + "%)" );

    // Apply the best power limit.
    m_nvml->setPowerLimit( m_deviceIndex, m_plBestMW );

    // If FPS at best power limit is still above the user's target,
    // power-limit reduction alone wasn't enough — continue with
    // frequency cap reduction to reach the target.
    if ( m_config.targetFpsEnabled
         && m_config.targetFpsValue > 0.0
         && m_plBestFps > m_config.targetFpsValue )
    {
      log( "AutoUV: power limit at floor (" + std::to_string( m_plBestMW / 1000 )
           + " W) but FPS still above target ("
           + std::to_string( static_cast< int >( m_plBestFps ) ) + " > "
           + std::to_string( static_cast< int >( m_config.targetFpsValue ) )
           + ") — continuing with frequency cap reduction" );
      enterCapReduction();
    }
    else
    {
      enterDone( true, "Power-limit mode complete: " + std::to_string( m_plBestMW / 1000 )
                 + " W (saved ~" + std::to_string( static_cast< int >( savedPct ) ) + "% power)" );
    }
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
  else if ( m_phase == UVPhase::CapReduction )
    m_stepDurationMs = m_config.capReductionWindowMs;
  else if ( m_phase == UVPhase::PowerLimitSweep )
    m_stepDurationMs = m_config.powerLimitTestMs;
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

  saveCheckpoint( true );
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
  unsigned int minClockMHz = static_cast< unsigned int >( capMHz );
  if ( m_originalGpuMinClockMHz.has_value() )
    minClockMHz = std::min( minClockMHz, *m_originalGpuMinClockMHz );

  bool ok = m_nvml->setGpuLockedClocks( m_deviceIndex,
                                         minClockMHz,
                                         static_cast< unsigned int >( capMHz ) );
  if ( !ok )
    log( "AutoUV: WARNING — failed to set GPU locked clocks min="
         + std::to_string( minClockMHz ) + " MHz max="
         + std::to_string( capMHz ) + " MHz" );
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
  m_originalGpuMinClockMHz.reset();
  m_originalCoreOffsetMHz.reset();

  if ( auto pl = m_nvml->getEnforcedPowerLimitW( m_deviceIndex ); pl && *pl > 0.0 )
    m_originalPowerLimitW = *pl;

  if ( auto state = m_nvml->getOCState( m_deviceIndex ); state.has_value() )
  {
    m_originalGpuLockedClocks = state->gpuLockedClocks;
    if ( state->gpuLockedClocks.has_value() )
      m_originalGpuMinClockMHz = state->gpuLockedClocks->first;
    else if ( state->gpuClockRange.has_value() )
      m_originalGpuMinClockMHz = state->gpuClockRange->first;

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

  // In power-limit mode on success, keep the reduced power limit.
  const bool keepPowerLimit = keepCurrentCap && m_config.powerLimitMode;
  if ( !keepPowerLimit && m_originalPowerLimitW.has_value() && *m_originalPowerLimitW > 0.0 )
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

bool AutoUndervoltWorker::isThermallyStable() const
{
  // Need at least 10 samples (~5 s at 500 ms poll) to judge slope.
  if ( m_soakTempSamples.size() < 10 )
    return false;

  // Use last 10 samples to compute temperature slope in °C/min.
  const size_t n = std::min< size_t >( m_soakTempSamples.size(), 20 );
  const size_t start = m_soakTempSamples.size() - n;
  const int first = m_soakTempSamples[start];
  const int last  = m_soakTempSamples.back();
  const double dtSec = static_cast< double >( n ) * m_config.pollIntervalMs / 1000.0;
  const double slopeCPerMin = ( dtSec > 0.0 )
    ? ( std::abs( last - first ) / dtSec ) * 60.0 : 999.0;

  return slopeCPerMin <= static_cast< double >( m_config.thermalSoakSlopeCPerMin );
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
  prog.currentCapMHz = ( m_phase == UVPhase::CapReduction )
                      ? m_capReductionCurrentMHz
                      : ( m_phase == UVPhase::OffsetSearching ) ? m_finalCapMHz : m_mid;
  prog.bestCapMHz    = m_bestCap;
  prog.currentOffsetMHz = ( m_phase == UVPhase::OffsetSearching ) ? m_offsetMid : m_bestOffset;
  prog.bestOffsetMHz = m_bestOffset;
  prog.targetFps     = m_config.targetFpsEnabled ? m_config.targetFpsValue : 0.0;
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

  // FPS/W efficiency
  prog.baselineFpsPerWatt = m_baselineFpsPerWatt;
  if ( prog.fps > 0.0 && prog.powerDrawW > 0 )
    prog.fpsPerWatt = prog.fps / static_cast< double >( prog.powerDrawW );
  else
    prog.fpsPerWatt = 0.0;

  // Power-limit progress — show current test value during sweep,
  // or the locked-in best value during subsequent phases in hybrid mode.
  if ( m_phase == UVPhase::PowerLimitSweep )
    prog.currentPowerLimitW = static_cast< int >( m_plCurrentMW / 1000 );
  else if ( m_config.powerLimitMode && m_plBestMW > 0 )
    prog.currentPowerLimitW = static_cast< int >( m_plBestMW / 1000 );
  else
    prog.currentPowerLimitW = 0;

  // Update in-game overlay via shared memory
  {
    UccOverlayData od{};
    od.active           = 1;
    od.mode             = 1; // Auto-Undervolt
    od.phase            = static_cast<uint8_t>( prog.phase );
    od.iteration        = prog.iteration;
    od.maxIterations    = prog.maxIterations;
    od.currentOffsetMHz = prog.currentCapMHz;  // cap being tested
    od.bestStableMHz    = prog.bestCapMHz;
    od.gpuClockMHz      = prog.gpuClockMHz;
    od.tempC            = prog.tempC;
    od.gpuUtilPct       = prog.gpuUtilPct;
    od.powerDrawW       = prog.powerDrawW;
    od.fps              = prog.fps;
    if ( !msg.empty() )
      std::strncpy( od.message, msg.c_str(), sizeof( od.message ) - 1 );
    OverlayShmWriter::instance().update( od );
  }

  emit progress( prog );

  saveCheckpoint( false );
}

void AutoUndervoltWorker::saveCheckpoint( bool force, const std::string &suspendReason )
{
  if ( m_phase == UVPhase::Idle || m_phase == UVPhase::Done )
    return;

  const auto now = std::chrono::steady_clock::now();
  if ( !force && m_lastCheckpointPersist.time_since_epoch().count() != 0 )
  {
    const auto deltaMs = std::chrono::duration_cast< std::chrono::milliseconds >( now - m_lastCheckpointPersist ).count();
    if ( deltaMs < 2000 )
      return;
  }

  try
  {
    nlohmann::json j;
    j["kind"] = "auto_undervolt";
    j["version"] = 1;
    j["phase"] = uvPhaseToString( m_phase );
    j["deviceIndex"] = m_deviceIndex;
    j["appName"] = m_appName;
    if ( !suspendReason.empty() )
      j["suspendReason"] = suspendReason;

    j["config"] = {
      { "baselineMs", m_config.baselineMs },
      { "searchTestMs", m_config.searchTestMs },
      { "validationMs", m_config.validationMs },
      { "settleMs", m_config.settleMs },
      { "pollIntervalMs", m_config.pollIntervalMs },
      { "fpsCompareWindowMs", m_config.fpsCompareWindowMs },
      { "thermalLimitC", m_config.thermalLimitC },
      { "fpsDropPct", m_config.fpsDropPct },
      { "stepMHz", m_config.stepMHz },
      { "offsetStepMHz", m_config.offsetStepMHz },
      { "maxCoreOffsetMHz", m_config.maxCoreOffsetMHz },
      { "minGpuUtilPct", m_config.minGpuUtilPct },
      { "minP0ResidencyPct", m_config.minP0ResidencyPct },
      { "clockDroopMHz", m_config.clockDroopMHz },
      { "maxDroopPct", m_config.maxDroopPct },
      { "maxPowerCapPct", m_config.maxPowerCapPct },
      { "maxThermalThrottlePct", m_config.maxThermalThrottlePct },
      { "maxEventPowerPct", m_config.maxEventPowerPct },
      { "maxEventThermalPct", m_config.maxEventThermalPct },
      { "maxViolationPowerPct", m_config.maxViolationPowerPct },
      { "maxViolationThermalPct", m_config.maxViolationThermalPct },
      { "maxNvapiLimiterPct", m_config.maxNvapiLimiterPct },
      { "maxStepRetries", m_config.maxStepRetries },
      { "safetyMarginMHz", m_config.safetyMarginMHz },
      { "targetFpsEnabled", m_config.targetFpsEnabled },
      { "targetFpsValue", m_config.targetFpsValue },
      { "capReductionStepMHz", m_config.capReductionStepMHz },
      { "capReductionFineStepMHz", m_config.capReductionFineStepMHz },
      { "capReductionWindowMs", m_config.capReductionWindowMs },
      { "thermalSoakMs", m_config.thermalSoakMs },
      { "thermalSoakSlopeCPerMin", m_config.thermalSoakSlopeCPerMin },
      { "extendedValidation", m_config.extendedValidation },
      { "extendedValidationMs", m_config.extendedValidationMs },
      { "maxFpsStddevPct", m_config.maxFpsStddevPct },
      { "min1PctLowRatio", m_config.min1PctLowRatio },
      { "maxVoltageRisePct", m_config.maxVoltageRisePct },
      { "maxValidationOscillations", m_config.maxValidationOscillations },
      { "powerLimitMode", m_config.powerLimitMode },
      { "powerLimitStepW", m_config.powerLimitStepW },
      { "powerLimitTestMs", m_config.powerLimitTestMs }
    };

    j["state"] = {
      { "baselineClockMHz", m_baselineClockMHz },
      { "baselineMaxClockMHz", m_baselineMaxClockMHz },
      { "baselineFps", m_baselineFps },
      { "targetFps", m_targetFps },
      { "fpsThreshold", m_fpsThreshold },
      { "iteration", m_iteration },
      { "maxIterations", m_maxIterations },
      { "bestCap", m_bestCap },
      { "bestOffset", m_bestOffset },
      { "mid", m_mid },
      { "finalCapMHz", m_finalCapMHz },
      { "capReductionCurrentMHz", m_capReductionCurrentMHz },
      { "capReductionPrevMHz", m_capReductionPrevMHz },
      { "searchCapMHz", m_searchCapMHz },
      { "searchOffsetMHz", m_searchOffsetMHz },
      { "stableCapMHz", m_stableCapMHz },
      { "stableOffsetMHz", m_stableOffsetMHz },
      { "prevSearchAvgFps", m_prevSearchAvgFps },
      { "lastAction", searchActionToString( m_lastAction ) },
      { "nextAction", searchActionToString( m_nextAction ) },
      { "blockedRaiseOffset", m_blockedRaiseOffset },
      { "blockedLowerCap", m_blockedLowerCap },
      { "baselinePowerW", m_baselinePowerW },
      { "baselineVoltageMv", m_baselineVoltageMv },
      { "baselineFpsPerWatt", m_baselineFpsPerWatt },
      { "capReductionFineMode", m_capReductionFineMode },
      { "validationCapDir", m_validationCapDir },
      { "validationOscillations", m_validationOscillations },
      { "plCurrentMW", m_plCurrentMW },
      { "plMinMW", m_plMinMW },
      { "plDefaultMW", m_plDefaultMW },
      { "plBestMW", m_plBestMW },
      { "plBestFps", m_plBestFps },
      { "plBaselineFps", m_plBaselineFps }
    };

    std::filesystem::create_directories( std::filesystem::path( AUTO_UV_CHECKPOINT_PATH ).parent_path() );
    const std::string tmpPath = std::string( AUTO_UV_CHECKPOINT_PATH ) + ".tmp";

    std::ofstream out( tmpPath, std::ios::trunc );
    if ( !out.is_open() )
      return;
    out << j.dump( 2 );
    out.close();

    std::filesystem::rename( tmpPath, AUTO_UV_CHECKPOINT_PATH );
    m_lastCheckpointPersist = now;
  }
  catch ( ... )
  {
  }
}

void AutoUndervoltWorker::clearCheckpoint()
{
  try
  {
    std::filesystem::remove( AUTO_UV_CHECKPOINT_PATH );
  }
  catch ( ... )
  {
  }
}

bool AutoUndervoltWorker::tryResumeFromCheckpoint( unsigned int deviceIndex )
{
  try
  {
    if ( !std::filesystem::exists( AUTO_UV_CHECKPOINT_PATH ) )
      return false;

    std::ifstream in( AUTO_UV_CHECKPOINT_PATH );
    if ( !in.is_open() )
      return false;

    nlohmann::json j;
    in >> j;

    if ( j.value( "kind", std::string() ) != "auto_undervolt" )
      return false;
    if ( j.value( "deviceIndex", static_cast< unsigned int >( 0 ) ) != deviceIndex )
      return false;

    const std::string checkpointApp = j.value( "appName", std::string() );
    if ( !checkpointApp.empty() && !m_appName.empty() && checkpointApp != m_appName )
      return false;
    if ( m_appName.empty() )
      m_appName = checkpointApp;

    if ( j.contains( "config" ) && j["config"].is_object() )
    {
      const auto &cfg = j["config"];
      m_config.baselineMs = cfg.value( "baselineMs", m_config.baselineMs );
      m_config.searchTestMs = cfg.value( "searchTestMs", m_config.searchTestMs );
      m_config.validationMs = cfg.value( "validationMs", m_config.validationMs );
      m_config.settleMs = cfg.value( "settleMs", m_config.settleMs );
      m_config.pollIntervalMs = cfg.value( "pollIntervalMs", m_config.pollIntervalMs );
      m_config.fpsCompareWindowMs = cfg.value( "fpsCompareWindowMs", m_config.fpsCompareWindowMs );
      m_config.thermalLimitC = cfg.value( "thermalLimitC", m_config.thermalLimitC );
      m_config.fpsDropPct = cfg.value( "fpsDropPct", m_config.fpsDropPct );
      m_config.stepMHz = cfg.value( "stepMHz", m_config.stepMHz );
      m_config.offsetStepMHz = cfg.value( "offsetStepMHz", m_config.offsetStepMHz );
      m_config.maxCoreOffsetMHz = cfg.value( "maxCoreOffsetMHz", m_config.maxCoreOffsetMHz );
      m_config.minGpuUtilPct = cfg.value( "minGpuUtilPct", m_config.minGpuUtilPct );
      m_config.minP0ResidencyPct = cfg.value( "minP0ResidencyPct", m_config.minP0ResidencyPct );
      m_config.clockDroopMHz = cfg.value( "clockDroopMHz", m_config.clockDroopMHz );
      m_config.maxDroopPct = cfg.value( "maxDroopPct", m_config.maxDroopPct );
      m_config.maxPowerCapPct = cfg.value( "maxPowerCapPct", m_config.maxPowerCapPct );
      m_config.maxThermalThrottlePct = cfg.value( "maxThermalThrottlePct", m_config.maxThermalThrottlePct );
      m_config.maxEventPowerPct = cfg.value( "maxEventPowerPct", m_config.maxEventPowerPct );
      m_config.maxEventThermalPct = cfg.value( "maxEventThermalPct", m_config.maxEventThermalPct );
      m_config.maxViolationPowerPct = cfg.value( "maxViolationPowerPct", m_config.maxViolationPowerPct );
      m_config.maxViolationThermalPct = cfg.value( "maxViolationThermalPct", m_config.maxViolationThermalPct );
      m_config.maxNvapiLimiterPct = cfg.value( "maxNvapiLimiterPct", m_config.maxNvapiLimiterPct );
      m_config.maxStepRetries = cfg.value( "maxStepRetries", m_config.maxStepRetries );
      m_config.safetyMarginMHz = cfg.value( "safetyMarginMHz", m_config.safetyMarginMHz );
      m_config.targetFpsEnabled = cfg.value( "targetFpsEnabled", m_config.targetFpsEnabled );
      m_config.targetFpsValue = cfg.value( "targetFpsValue", m_config.targetFpsValue );
      m_config.capReductionStepMHz = cfg.value( "capReductionStepMHz", m_config.capReductionStepMHz );
      m_config.capReductionFineStepMHz = cfg.value( "capReductionFineStepMHz", m_config.capReductionFineStepMHz );
      m_config.capReductionWindowMs = cfg.value( "capReductionWindowMs", m_config.capReductionWindowMs );
      m_config.thermalSoakMs = cfg.value( "thermalSoakMs", m_config.thermalSoakMs );
      m_config.thermalSoakSlopeCPerMin = cfg.value( "thermalSoakSlopeCPerMin", m_config.thermalSoakSlopeCPerMin );
      m_config.extendedValidation = cfg.value( "extendedValidation", m_config.extendedValidation );
      m_config.extendedValidationMs = cfg.value( "extendedValidationMs", m_config.extendedValidationMs );
      m_config.maxFpsStddevPct = cfg.value( "maxFpsStddevPct", m_config.maxFpsStddevPct );
      m_config.min1PctLowRatio = cfg.value( "min1PctLowRatio", m_config.min1PctLowRatio );
      m_config.maxVoltageRisePct = cfg.value( "maxVoltageRisePct", m_config.maxVoltageRisePct );
      m_config.maxValidationOscillations = cfg.value( "maxValidationOscillations", m_config.maxValidationOscillations );
      m_config.powerLimitMode = cfg.value( "powerLimitMode", m_config.powerLimitMode );
      m_config.powerLimitStepW = cfg.value( "powerLimitStepW", m_config.powerLimitStepW );
      m_config.powerLimitTestMs = cfg.value( "powerLimitTestMs", m_config.powerLimitTestMs );
    }

    if ( j.contains( "state" ) && j["state"].is_object() )
    {
      const auto &s = j["state"];
      m_baselineClockMHz = s.value( "baselineClockMHz", 0 );
      m_baselineMaxClockMHz = s.value( "baselineMaxClockMHz", 0 );
      m_baselineFps = s.value( "baselineFps", 0.0 );
      m_targetFps = s.value( "targetFps", 0.0 );
      m_fpsThreshold = s.value( "fpsThreshold", 0.0 );
      m_iteration = s.value( "iteration", 0 );
      m_maxIterations = s.value( "maxIterations", 0 );
      m_bestCap = s.value( "bestCap", 0 );
      m_bestOffset = s.value( "bestOffset", 0 );
      m_mid = s.value( "mid", 0 );
      m_finalCapMHz = s.value( "finalCapMHz", 0 );
      m_capReductionCurrentMHz = s.value( "capReductionCurrentMHz", 0 );
      m_capReductionPrevMHz = s.value( "capReductionPrevMHz", 0 );
      m_searchCapMHz = s.value( "searchCapMHz", 0 );
      m_searchOffsetMHz = s.value( "searchOffsetMHz", 0 );
      m_stableCapMHz = s.value( "stableCapMHz", 0 );
      m_stableOffsetMHz = s.value( "stableOffsetMHz", 0 );
      m_prevSearchAvgFps = s.value( "prevSearchAvgFps", 0.0 );
      m_lastAction = searchActionFromString( s.value( "lastAction", std::string( "none" ) ) );
      m_nextAction = searchActionFromString( s.value( "nextAction", std::string( "raise_offset" ) ) );
      m_blockedRaiseOffset = s.value( "blockedRaiseOffset", false );
      m_blockedLowerCap = s.value( "blockedLowerCap", true );
      m_baselinePowerW = s.value( "baselinePowerW", 0.0 );
      m_baselineVoltageMv = s.value( "baselineVoltageMv", 0.0 );
      m_baselineFpsPerWatt = s.value( "baselineFpsPerWatt", 0.0 );
      m_capReductionFineMode = s.value( "capReductionFineMode", false );
      m_validationCapDir = s.value( "validationCapDir", 0 );
      m_validationOscillations = s.value( "validationOscillations", 0 );
      m_plCurrentMW = s.value( "plCurrentMW", static_cast< unsigned int >( 0 ) );
      m_plMinMW = s.value( "plMinMW", static_cast< unsigned int >( 0 ) );
      m_plDefaultMW = s.value( "plDefaultMW", static_cast< unsigned int >( 0 ) );
      m_plBestMW = s.value( "plBestMW", static_cast< unsigned int >( 0 ) );
      m_plBestFps = s.value( "plBestFps", 0.0 );
      m_plBaselineFps = s.value( "plBaselineFps", 0.0 );
    }

    const UVPhase checkpointPhase = uvPhaseFromString( j.value( "phase", std::string( "idle" ) ) );
    if ( checkpointPhase == UVPhase::Idle || checkpointPhase == UVPhase::Done )
      return false;

    log( "AutoUV: resuming interrupted session from phase '" + std::string( uvPhaseToString( checkpointPhase ) ) + "'" );

    switch ( checkpointPhase )
    {
    case UVPhase::Baseline:
      enterBaseline();
      return true;

    case UVPhase::CapReduction:
      m_phase = UVPhase::CapReduction;
      ensureMaxPowerLimit();
      if ( m_capReductionCurrentMHz <= 0 )
        m_capReductionCurrentMHz = std::max( m_config.capReductionStepMHz, m_baselineClockMHz );
      applyFreqCap( m_capReductionCurrentMHz );
      applyCoreOffset( 0 );
      resetStepMetrics();
      emitProgress( "Resuming cap reduction — start the game/application now so FPS data is available." );
      m_pollTimer->start( m_config.pollIntervalMs );
      return true;

    case UVPhase::Searching:
      // StepBack: If the interruption happened while testing a raised offset,
      // treat that candidate as unstable and continue from the last known-stable
      // offset rather than retrying the same risky point.
      // RepeatStep: User quit voluntarily — just repeat the same step.
      if ( m_resumeMode == ResumeMode::StepBack
           && m_searchOffsetMHz > m_stableOffsetMHz && m_stableOffsetMHz >= 0 )
      {
        log( "AutoUV: resume (step-back) detected interrupted raised-offset step (+"
             + std::to_string( m_searchOffsetMHz ) + " MHz); using last stable +"
             + std::to_string( m_stableOffsetMHz ) + " MHz" );

        m_blockedRaiseOffset = true;
        m_searchCapMHz = std::max( 1, m_baselineClockMHz );
        m_searchOffsetMHz = m_stableOffsetMHz;
        m_bestCap = ( m_stableCapMHz > 0 ) ? m_stableCapMHz : m_searchCapMHz;
        m_bestOffset = m_stableOffsetMHz;

        applyFreqCap( m_searchCapMHz );
        applyCoreOffset( m_searchOffsetMHz );
        enterValidation();
        m_pollTimer->start( m_config.pollIntervalMs );
        saveCheckpoint( true );
        return true;
      }

      if ( m_resumeMode == ResumeMode::RepeatStep )
        log( "AutoUV: resume (repeat-step) — retrying last step" );

      m_phase = UVPhase::Searching;
      ensureMaxPowerLimit();
      if ( m_searchCapMHz <= 0 )
        m_searchCapMHz = std::max( 1, m_baselineClockMHz );
      applyFreqCap( m_searchCapMHz );
      applyCoreOffset( std::max( 0, m_searchOffsetMHz ) );
      resetStepMetrics();
      emitProgress( "Resuming offset search — start the game/application now so FPS data is available." );
      m_pollTimer->start( m_config.pollIntervalMs );
      return true;

    case UVPhase::Validating:
      m_phase = UVPhase::Validating;
      if ( m_mid <= 0 )
        m_mid = std::max( 1, m_baselineClockMHz );
      applyFreqCap( m_mid );
      applyCoreOffset( std::max( 0, m_bestOffset ) );
      resetStepMetrics();
      m_stepDurationMs = m_config.validationMs;
      emitProgress( "Resuming validation — start the game/application now so FPS data is available." );
      m_pollTimer->start( m_config.pollIntervalMs );
      return true;

    case UVPhase::PowerLimitSweep:
      m_phase = UVPhase::PowerLimitSweep;
      if ( m_plCurrentMW > 0 )
        m_nvml->setPowerLimit( m_deviceIndex, m_plCurrentMW );
      resetStepMetrics();
      m_stepDurationMs = m_config.powerLimitTestMs;
      emitProgress( "Resuming power-limit sweep — start the game/application now." );
      m_pollTimer->start( m_config.pollIntervalMs );
      return true;

    default:
      break;
    }
  }
  catch ( const std::exception &e )
  {
    log( std::string( "AutoUV: checkpoint resume failed: " ) + e.what() );
  }

  return false;
}
