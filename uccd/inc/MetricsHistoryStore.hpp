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

#include <cstdint>
#include <cstring>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <sstream>
#include <vector>
#include <algorithm>

/**
 * @brief A single timestamped data point.
 */
struct MetricDataPoint
{
  int64_t timestampMs;  ///< Unix epoch milliseconds
  double  value;
};

/**
 * @brief Thread-safe, string-keyed ring buffer for hardware monitoring metrics.
 *
 * Any number of named series can be pushed.  Workers push data from their own
 * threads; the D-Bus adaptor reads via querySince().  A shared_mutex allows
 * concurrent readers with exclusive writers.
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
   * @brief Push a new data point for the given metric key.
   *
   * Automatically trims points outside the configured horizon.
   * Thread-safe (exclusive lock).
   */
  void push( const std::string &key, int64_t timestampMs, double value )
  {
    std::unique_lock lock( m_mutex );
    auto &buf = m_buffers[ key ];
    buf.push_back( { timestampMs, value } );
    trim( buf, timestampMs );
  }

  /**
   * @brief Convenience overload using the current wall-clock time.
   */
  void push( const std::string &key, double value )
  {
    const auto now = std::chrono::duration_cast< std::chrono::milliseconds >(
      std::chrono::system_clock::now().time_since_epoch() ).count();
    push( key, now, value );
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
   *   "someKey": [[ts, val], [ts, val], ...],
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

    for ( const auto &[key, buf] : m_buffers )
    {
      if ( buf.empty() )
        continue;

      auto it = std::lower_bound(
        buf.begin(), buf.end(), sinceMs,
        []( const MetricDataPoint &pt, int64_t ts ) { return pt.timestampMs < ts; } );

      if ( it == buf.end() )
        continue;

      if ( !firstMetric )
        os << ',';
      firstMetric = false;

      os << '"' << key << "\":[";
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
   *     uint16_t keyLen          (length of key string, no NUL)
   *     char     key[keyLen]     (key bytes)
   *     uint32_t count           (number of data points)
   *     count × { int64_t timestampMs, double value }   (16 bytes each)
   * @endcode
   *
   * Empty series are omitted.  The caller detects end-of-data by consuming
   * exactly (2 + keyLen + 4 + count * 16) bytes per block until the buffer
   * is exhausted.
   */
  [[nodiscard]] std::vector< uint8_t > querySinceBinary( int64_t sinceMs ) const
  {
    std::shared_lock lock( m_mutex );

    std::vector< uint8_t > out;
    out.reserve( 4096 );

    for ( const auto &[key, buf] : m_buffers )
    {
      if ( buf.empty() )
        continue;

      auto it = std::lower_bound(
        buf.begin(), buf.end(), sinceMs,
        []( const MetricDataPoint &pt, int64_t ts ) { return pt.timestampMs < ts; } );

      if ( it == buf.end() )
        continue;

      const uint32_t count = static_cast< uint32_t >( std::distance( it, buf.end() ) );

      // --- header: keyLen (2 bytes) + key chars + count (4 bytes) ---
      const uint16_t keyLen = static_cast< uint16_t >( key.size() );
      out.insert( out.end(),
                  reinterpret_cast< const uint8_t * >( &keyLen ),
                  reinterpret_cast< const uint8_t * >( &keyLen ) + sizeof( keyLen ) );
      out.insert( out.end(), key.begin(), key.end() );
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

  /**
   * @brief Return all keys that currently have at least one data point.
   */
  [[nodiscard]] std::vector< std::string > activeKeys() const
  {
    std::shared_lock lock( m_mutex );
    std::vector< std::string > keys;
    keys.reserve( m_buffers.size() );
    for ( const auto &[k, buf] : m_buffers )
    {
      if ( !buf.empty() )
        keys.push_back( k );
    }
    return keys;
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
  std::map< std::string, std::deque< MetricDataPoint > > m_buffers;
  int64_t m_horizonMs = static_cast< int64_t >( DEFAULT_HORIZON_S ) * 1000;
};
