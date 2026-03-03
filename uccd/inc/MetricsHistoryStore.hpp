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

#include <array>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <sstream>
#include <vector>
#include <algorithm>

/**
 * @brief Identifiers for each tracked metric.
 *
 * The underlying value is used as an index into the per-metric ring buffers.
 */
enum class MetricId : uint8_t
{
  CpuTemp,
  CpuFanDuty,
  CpuPower,
  CpuFrequency,
  GpuTemp,
  GpuFanDuty,
  GpuPower,
  GpuFrequency,
  GpuVramFrequency,
  GpuCoreVoltage,
  Count  ///< Sentinel — must be last
};

/**
 * @brief Human-readable name for a metric (matches JSON key).
 */
constexpr const char *metricName( MetricId id ) noexcept
{
  switch ( id )
  {
    case MetricId::CpuTemp:             return "cpuTemp";
    case MetricId::CpuFanDuty:          return "cpuFanDuty";
    case MetricId::CpuPower:            return "cpuPower";
    case MetricId::CpuFrequency:        return "cpuFrequency";
    case MetricId::GpuTemp:             return "gpuTemp";
    case MetricId::GpuFanDuty:          return "gpuFanDuty";
    case MetricId::GpuPower:            return "gpuPower";
    case MetricId::GpuFrequency:        return "gpuFrequency";
    case MetricId::GpuVramFrequency:    return "gpuVramFrequency";
    case MetricId::GpuCoreVoltage:      return "gpuCoreVoltage";
    default:                            return "unknown";
  }
}

/**
 * @brief A single timestamped data point.
 */
struct MetricDataPoint
{
  int64_t timestampMs;  ///< Unix epoch milliseconds
  double  value;
};

/**
 * @brief Thread-safe ring buffer for hardware monitoring metrics.
 *
 * Workers push data from their own threads; the D-Bus adaptor reads via
 * querySince().  A shared_mutex allows concurrent readers with exclusive
 * writers (each push only locks one metric's deque, so contention is minimal).
 *
 * Eviction is age-based: points older than the configured horizon are pruned
 * on every push().
 */
class MetricsHistoryStore
{
public:
  static constexpr int DEFAULT_HORIZON_S = 1800;  ///< 30 minutes
  static constexpr int MIN_HORIZON_S     = 60;
  static constexpr int MAX_HORIZON_S     = 7200;  ///< 2 hours

  MetricsHistoryStore() = default;

  // -----------------------------------------------------------------------
  // Writer API (called from worker threads)
  // -----------------------------------------------------------------------

  /**
   * @brief Push a new data point for the given metric.
   *
   * Automatically trims points outside the configured horizon.
   * Thread-safe (exclusive lock on the metric's deque).
   */
  void push( MetricId id, int64_t timestampMs, double value )
  {
    const auto idx = static_cast< size_t >( id );
    if ( idx >= static_cast< size_t >( MetricId::Count ) )
      return;

    std::unique_lock lock( m_mutex );
    auto &buf = m_buffers[ idx ];
    buf.push_back( { timestampMs, value } );
    trim( buf, timestampMs );
  }

  /**
   * @brief Convenience overload using the current wall-clock time.
   */
  void push( MetricId id, double value )
  {
    const auto now = std::chrono::duration_cast< std::chrono::milliseconds >(
      std::chrono::system_clock::now().time_since_epoch() ).count();
    push( id, now, value );
  }

  // -----------------------------------------------------------------------
  // Reader API (called from D-Bus thread)
  // -----------------------------------------------------------------------

  /**
   * @brief Serialize all metrics with timestamps >= sinceMs to a JSON string.
   *
   * Output format:
   * @code
   * {
   *   "cpuTemp": [[ts, val], [ts, val], ...],
   *   "cpuFanDuty": [[ts, val], ...],
   *   ...
   * }
   * @endcode
   *
   * Empty series are omitted.
   */
  [[nodiscard]] std::string querySinceJSON( int64_t sinceMs ) const
  {
    std::shared_lock lock( m_mutex );

    std::ostringstream os;
    os << '{';
    bool firstMetric = true;

    for ( size_t i = 0; i < static_cast< size_t >( MetricId::Count ); ++i )
    {
      const auto &buf = m_buffers[ i ];
      if ( buf.empty() )
        continue;

      // Binary search for the first element >= sinceMs
      auto it = std::lower_bound(
        buf.begin(), buf.end(), sinceMs,
        []( const MetricDataPoint &pt, int64_t ts ) { return pt.timestampMs < ts; } );

      if ( it == buf.end() )
        continue;

      if ( !firstMetric )
        os << ',';
      firstMetric = false;

      os << '"' << metricName( static_cast< MetricId >( i ) ) << "\":[";
      bool firstPt = true;
      for ( ; it != buf.end(); ++it )
      {
        if ( !firstPt )
          os << ',';
        firstPt = false;
        os << '[' << it->timestampMs << ',' << it->value << ']';
      }
      os << ']';
    }
    os << '}';
    return os.str();
  }

  /**
   * @brief Serialize all metrics with timestamps >= sinceMs to a compact binary blob.
   *
   * Wire layout (native endian — same-host IPC only):
   * @code
   *   Repeated for each non-empty metric series:
   *     uint8_t  metricId
   *     uint32_t count         (number of data points)
   *     count × { int64_t timestampMs, double value }   (16 bytes each)
   * @endcode
   *
   * Empty series are omitted.  The caller detects end-of-data by consuming
   * exactly (1 + 4 + count * 16) bytes per block until the buffer is exhausted.
   */
  [[nodiscard]] std::vector< uint8_t > querySinceBinary( int64_t sinceMs ) const
  {
    std::shared_lock lock( m_mutex );

    std::vector< uint8_t > out;
    out.reserve( 2048 );

    for ( size_t i = 0; i < static_cast< size_t >( MetricId::Count ); ++i )
    {
      const auto &buf = m_buffers[ i ];
      if ( buf.empty() )
        continue;

      auto it = std::lower_bound(
        buf.begin(), buf.end(), sinceMs,
        []( const MetricDataPoint &pt, int64_t ts ) { return pt.timestampMs < ts; } );

      if ( it == buf.end() )
        continue;

      const uint32_t count = static_cast< uint32_t >( std::distance( it, buf.end() ) );

      // --- header: metricId (1 byte) + count (4 bytes) ---
      const uint8_t id = static_cast< uint8_t >( i );
      out.push_back( id );
      out.insert( out.end(),
                  reinterpret_cast< const uint8_t * >( &count ),
                  reinterpret_cast< const uint8_t * >( &count ) + sizeof( count ) );

      // --- data points: int64_t ts + double value (16 bytes each) ---
      for ( ; it != buf.end(); ++it )
      {
        out.insert( out.end(),
                    reinterpret_cast< const uint8_t * >( &it->timestampMs ),
                    reinterpret_cast< const uint8_t * >( &it->timestampMs ) + sizeof( it->timestampMs ) );
        out.insert( out.end(),
                    reinterpret_cast< const uint8_t * >( &it->value ),
                    reinterpret_cast< const uint8_t * >( &it->value ) + sizeof( it->value ) );
      }
    }

    return out;
  }

  // -----------------------------------------------------------------------
  // Configuration
  // -----------------------------------------------------------------------

  /**
   * @brief Set the history horizon in seconds.
   *
   * Points older than (now – horizon) will be evicted on the next push().
   */
  void setHorizon( int seconds )
  {
    std::unique_lock lock( m_mutex );
    m_horizonMs = static_cast< int64_t >(
      std::clamp( seconds, MIN_HORIZON_S, MAX_HORIZON_S ) ) * 1000;
  }

  [[nodiscard]] int horizonSeconds() const
  {
    std::shared_lock lock( m_mutex );
    return static_cast< int >( m_horizonMs / 1000 );
  }

private:
  void trim( std::deque< MetricDataPoint > &buf, int64_t nowMs ) const
  {
    const int64_t cutoff = nowMs - m_horizonMs;
    while ( !buf.empty() && buf.front().timestampMs < cutoff )
      buf.pop_front();
  }

  mutable std::shared_mutex m_mutex;
  std::array< std::deque< MetricDataPoint >,
              static_cast< size_t >( MetricId::Count ) > m_buffers;
  int64_t m_horizonMs = static_cast< int64_t >( DEFAULT_HORIZON_S ) * 1000;
};
