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

#include "workers/NvidiaOCWorker.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <map>
#include <syslog.h>

NvidiaOCWorker::NvidiaOCWorker( std::shared_ptr< NvmlWrapper > nvml,
                                std::function< void( const std::string & ) > logFunction )
  : m_nvml( std::move( nvml ) )
  , m_logFunction( std::move( logFunction ) )
{
  if ( m_nvml && m_nvml->isInitialized() )
    log( "NvidiaOCWorker: NVML initialised, " + std::to_string( m_nvml->deviceCount() ) + " GPU(s)" );
  else
    log( "NvidiaOCWorker: NVML not available" );
}

bool NvidiaOCWorker::isAvailable() const noexcept
{
  return m_nvml && m_nvml->isInitialized() && m_nvml->deviceCount() > 0;
}

std::string NvidiaOCWorker::getOCStateJSON( unsigned int deviceIndex ) const
{
  if ( !isAvailable() )
    return "{}";

  auto stateOpt = m_nvml->getOCState( deviceIndex );
  if ( !stateOpt )
    return "{}";

  const auto &s = *stateOpt;
  QJsonObject root;
  root["gpuName"] = QString::fromStdString( s.gpuName );
  root["tempC"] = static_cast< int >( s.tempC );
  root["tempShutdownC"] = static_cast< int >( s.tempShutdownC );
  root["powerDrawW"] = s.powerDrawW;
  root["powerLimitW"] = s.powerLimitW;
  root["powerDefaultW"] = s.powerDefaultW;
  root["powerMinW"] = s.powerMinW;
  root["powerMaxW"] = s.powerMaxW;
  root["offsetsSupported"] = s.offsetsSupported;
  root["lockedClocksSupported"] = s.lockedClocksSupported;

  // GPU clock range
  if ( s.gpuClockRange )
  {
    QJsonObject range;
    range["min"] = static_cast< int >( s.gpuClockRange->first );
    range["max"] = static_cast< int >( s.gpuClockRange->second );
    root["gpuClockRange"] = range;
  }
  if ( s.vramClockRange )
  {
    QJsonObject range;
    range["min"] = static_cast< int >( s.vramClockRange->first );
    range["max"] = static_cast< int >( s.vramClockRange->second );
    root["vramClockRange"] = range;
  }

  // Unified P-states array — each entry has GPU and VRAM clock info
  // Build a map of pstate index → {gpu info, vram info}
  std::map< unsigned int, std::pair< const NvmlPStateClockInfo*, const NvmlPStateClockInfo* > > pstateMap;
  for ( const auto &ps : s.gpuPStates )
    pstateMap[ps.pstate].first = &ps;
  for ( const auto &ps : s.vramPStates )
    pstateMap[ps.pstate].second = &ps;

  QJsonArray pstates;
  for ( const auto &[psIdx, pair] : pstateMap )
  {
    QJsonObject o;
    o["pstate"] = static_cast< int >( psIdx );

    if ( pair.first )
    {
      QJsonObject gpu;
      gpu["minMHz"] = static_cast< int >( pair.first->minMHz );
      gpu["maxMHz"] = static_cast< int >( pair.first->maxMHz );
      gpu["currentOffset"] = pair.first->currentOffset;
      gpu["minOffset"] = pair.first->minOffset;
      gpu["maxOffset"] = pair.first->maxOffset;
      gpu["offsetWritable"] = pair.first->offsetWritable;
      o["gpu"] = gpu;
    }
    if ( pair.second )
    {
      QJsonObject vram;
      vram["minMHz"] = static_cast< int >( pair.second->minMHz );
      vram["maxMHz"] = static_cast< int >( pair.second->maxMHz );
      vram["currentOffset"] = pair.second->currentOffset;
      vram["minOffset"] = pair.second->minOffset;
      vram["maxOffset"] = pair.second->maxOffset;
      vram["offsetWritable"] = pair.second->offsetWritable;
      o["vram"] = vram;
    }

    pstates.append( o );
  }
  root["pstates"] = pstates;

  // Locked clocks
  if ( s.gpuLockedClocks )
  {
    QJsonObject lc;
    lc["min"] = static_cast< int >( s.gpuLockedClocks->first );
    lc["max"] = static_cast< int >( s.gpuLockedClocks->second );
    root["gpuLockedClocks"] = lc;
  }
  if ( s.vramLockedClocks )
  {
    QJsonObject lc;
    lc["min"] = static_cast< int >( s.vramLockedClocks->first );
    lc["max"] = static_cast< int >( s.vramLockedClocks->second );
    root["vramLockedClocks"] = lc;
  }

  QJsonDocument doc( root );
  return doc.toJson( QJsonDocument::Compact ).toStdString();
}

bool NvidiaOCWorker::setClockOffset( unsigned int deviceIndex,
                                     unsigned int clockType,
                                     unsigned int pstate,
                                     int offsetMHz )
{
  if ( !isAvailable() )
    return false;

  auto ct = static_cast< nvml::nvmlClockType_t >( clockType );
  auto ps = static_cast< nvml::nvmlPstates_t >( pstate );

  if ( !m_nvml->isClockOffsetWritable( deviceIndex, ct, ps ) && offsetMHz == 0 )
    return true;

  bool ok = m_nvml->setClockOffset( deviceIndex, ct, ps, offsetMHz );
  if ( ok )
    log( "Set clock offset: type=" + std::to_string( clockType ) +
         " pstate=" + std::to_string( pstate ) +
         " offset=" + std::to_string( offsetMHz ) + " MHz" );
  return ok;
}

bool NvidiaOCWorker::setGpuLockedClocks( unsigned int deviceIndex,
                                         unsigned int minMHz,
                                         unsigned int maxMHz )
{
  if ( !isAvailable() )
    return false;

  bool ok = m_nvml->setGpuLockedClocks( deviceIndex, minMHz, maxMHz );
  if ( ok )
    log( "Set GPU locked clocks: " + std::to_string( minMHz ) + "-" + std::to_string( maxMHz ) + " MHz" );
  return ok;
}

bool NvidiaOCWorker::setVramLockedClocks( unsigned int deviceIndex,
                                          unsigned int minMHz,
                                          unsigned int maxMHz )
{
  if ( !isAvailable() )
    return false;

  bool ok = m_nvml->setVramLockedClocks( deviceIndex, minMHz, maxMHz );
  if ( ok )
    log( "Set VRAM locked clocks: " + std::to_string( minMHz ) + "-" + std::to_string( maxMHz ) + " MHz" );
  return ok;
}

bool NvidiaOCWorker::resetGpuLockedClocks( unsigned int deviceIndex )
{
  if ( !isAvailable() )
    return false;
  return m_nvml->resetGpuLockedClocks( deviceIndex );
}

bool NvidiaOCWorker::resetVramLockedClocks( unsigned int deviceIndex )
{
  if ( !isAvailable() )
    return false;
  return m_nvml->resetVramLockedClocks( deviceIndex );
}

bool NvidiaOCWorker::resetAllClockOffsets( unsigned int deviceIndex )
{
  if ( !isAvailable() )
    return false;
  return m_nvml->resetAllClockOffsets( deviceIndex );
}

bool NvidiaOCWorker::setPowerLimit( unsigned int deviceIndex, double watts )
{
  if ( !isAvailable() )
    return false;

  auto mw = static_cast< unsigned int >( watts * 1000.0 );
  bool ok = m_nvml->setPowerLimit( deviceIndex, mw );
  if ( ok )
    log( "Set GPU power limit: " + std::to_string( watts ) + " W" );
  return ok;
}

bool NvidiaOCWorker::resetPowerLimit( unsigned int deviceIndex )
{
  if ( !isAvailable() )
    return false;
  return m_nvml->resetPowerLimit( deviceIndex );
}

bool NvidiaOCWorker::applyGpuOCProfile( const std::string &profileJSON, unsigned int deviceIndex )
{
  if ( !isAvailable() || profileJSON.empty() || profileJSON == "{}" )
    return false;

  QJsonDocument doc = QJsonDocument::fromJson( QByteArray::fromStdString( profileJSON ) );
  if ( !doc.isObject() )
    return false;

  QJsonObject obj = doc.object();
  bool anyFailed = false;

  auto applyOffsets = [this, deviceIndex, &anyFailed]( const QJsonArray &arr,
                                                        unsigned int clockType,
                                                        const char *name ) {
    bool requestedNonZero = false;
    bool appliedNonZero = false;
    bool requestedP0NonZero = false;
    bool appliedP0NonZero = false;

    auto applyEntry = [this, deviceIndex, clockType, name,
                       &requestedNonZero, &appliedNonZero,
                       &requestedP0NonZero, &appliedP0NonZero]( const QJsonObject &o ) {
      const unsigned int ps = static_cast< unsigned int >( o["pstate"].toInt() );
      const int offset = o["offsetMHz"].toInt();

      const bool ok = setClockOffset( deviceIndex, clockType, ps, offset );
      if ( ok && offset != 0 )
      {
        appliedNonZero = true;
        if ( ps == 0 )
          appliedP0NonZero = true;
      }

      if ( offset != 0 )
      {
        requestedNonZero = true;
        if ( ps == 0 )
          requestedP0NonZero = true;

        if ( !ok )
        {
          log( std::string( "Offset write rejected for " ) + name +
               " pstate=" + std::to_string( ps ) +
               " offset=" + std::to_string( offset ) + " MHz" );
        }
      }
    };

    // Apply P0 first when present so user-requested performance-state offset
    // is prioritized and its failure is visible to the caller.
    for ( const auto &v : arr )
    {
      QJsonObject o = v.toObject();
      const unsigned int ps = static_cast< unsigned int >( o["pstate"].toInt() );
      if ( ps == 0 )
        applyEntry( o );
    }

    for ( const auto &v : arr )
    {
      QJsonObject o = v.toObject();
      const unsigned int ps = static_cast< unsigned int >( o["pstate"].toInt() );
      if ( ps != 0 )
        applyEntry( o );
    }

    if ( requestedP0NonZero && !appliedP0NonZero )
      anyFailed = true;

    if ( requestedNonZero && !appliedNonZero )
      anyFailed = true;
  };

  // Apply unified offsets format
  if ( obj.contains( "offsets" ) && obj["offsets"].isArray() )
  {
    QJsonArray coreOffsets;
    QJsonArray vramOffsets;

    for ( const auto &v : obj["offsets"].toArray() )
    {
      QJsonObject o = v.toObject();
      const int pstate = o["pstate"].toInt();

      if ( o.contains( "gpuOffsetMHz" ) )
      {
        QJsonObject coreEntry;
        coreEntry["pstate"] = pstate;
        coreEntry["offsetMHz"] = o["gpuOffsetMHz"].toInt();
        coreOffsets.append( coreEntry );
      }

      if ( o.contains( "vramOffsetMHz" ) )
      {
        QJsonObject vramEntry;
        vramEntry["pstate"] = pstate;
        vramEntry["offsetMHz"] = o["vramOffsetMHz"].toInt();
        vramOffsets.append( vramEntry );
      }
    }

    if ( !coreOffsets.isEmpty() )
      applyOffsets( coreOffsets, nvml::NVML_CLOCK_GRAPHICS, "graphics" );

    if ( !vramOffsets.isEmpty() )
      applyOffsets( vramOffsets, nvml::NVML_CLOCK_MEM, "memory" );
  }

  // Apply GPU locked clocks
  if ( obj.contains( "gpuLockedClocks" ) && obj["gpuLockedClocks"].isObject() )
  {
    QJsonObject lc = obj["gpuLockedClocks"].toObject();
    if ( lc["enabled"].toBool( false ) )
    {
      unsigned int lo = static_cast< unsigned int >( lc["min"].toInt() );
      unsigned int hi = static_cast< unsigned int >( lc["max"].toInt() );
      if ( !setGpuLockedClocks( deviceIndex, lo, hi ) )
        anyFailed = true;
    }
    else
    {
      resetGpuLockedClocks( deviceIndex );
    }
  }

  // Apply VRAM locked clocks
  if ( obj.contains( "vramLockedClocks" ) && obj["vramLockedClocks"].isObject() )
  {
    QJsonObject lc = obj["vramLockedClocks"].toObject();
    if ( lc["enabled"].toBool( false ) )
    {
      unsigned int lo = static_cast< unsigned int >( lc["min"].toInt() );
      unsigned int hi = static_cast< unsigned int >( lc["max"].toInt() );
      if ( !setVramLockedClocks( deviceIndex, lo, hi ) )
        anyFailed = true;
    }
    else
    {
      resetVramLockedClocks( deviceIndex );
    }
  }

  // Apply power limit
  if ( obj.contains( "powerLimitW" ) )
  {
    double pw = obj["powerLimitW"].toDouble( 0.0 );
    if ( pw > 0.0 )
    {
      if ( !setPowerLimit( deviceIndex, pw ) )
        anyFailed = true;
    }
    else
    {
      resetPowerLimit( deviceIndex );
    }
  }

  return !anyFailed;
}

bool NvidiaOCWorker::resetAll( unsigned int deviceIndex )
{
  if ( !isAvailable() )
    return false;

  bool ok = true;
  if ( !m_nvml->resetAllClockOffsets( deviceIndex ) ) ok = false;
  if ( !m_nvml->resetGpuLockedClocks( deviceIndex ) ) ok = false;
  if ( !m_nvml->resetVramLockedClocks( deviceIndex ) ) ok = false;
  if ( !m_nvml->resetPowerLimit( deviceIndex ) ) ok = false;
  if ( ok )
    log( "Reset all GPU OC settings to defaults" );
  return ok;
}

void NvidiaOCWorker::log( const std::string &msg ) const
{
  if ( m_logFunction )
    m_logFunction( msg );
  syslog( LOG_INFO, "%s", msg.c_str() );
}
