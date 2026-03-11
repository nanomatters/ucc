/*
 * Unit tests for MetricsHistoryStore – push, querySinceJSON,
 * horizon clamping, eviction, and querySinceBinary().
 */

#include <QTest>
#include <cstring>
#include <string>
#include "MetricsHistoryStore.hpp"

// C++20 helper — std::string::contains() is C++23
static bool strContains( const std::string &haystack, const char *needle )
{
  return haystack.find( needle ) != std::string::npos;
}

class TestMetricsHistory : public QObject
{
  Q_OBJECT

private slots:

  // ---- push + querySinceJSON() -----------------------------------------

  void pushAndQuery_basic()
  {
    MetricsHistoryStore store;
    store.push( "cpuTemp", 1000, 45.5 );
    store.push( "cpuTemp", 2000, 46.0 );

    std::string json = store.querySinceJSON( 0 );

    // Should contain both data points
    QVERIFY( strContains( json,  "cpuTemp" ) );
    QVERIFY( strContains( json,  "45.5" ) );
    QVERIFY( strContains( json,  "46" ) );
  }

  void pushAndQuery_sinceFilters()
  {
    MetricsHistoryStore store;
    store.push( "cpuTemp", 1000, 10.0 );
    store.push( "cpuTemp", 2000, 20.0 );
    store.push( "cpuTemp", 3000, 30.0 );

    // Query since ts=2000 → should include 2000 and 3000 but not 1000
    std::string json = store.querySinceJSON( 2000 );
    QVERIFY( strContains( json,  "20" ) );
    QVERIFY( strContains( json,  "30" ) );
    // Instead, query since 2500 — only the 3000 point should remain
    std::string json2 = store.querySinceJSON( 2500 );
    QVERIFY( strContains( json2,  "30" ) );
    // 20.0 data point should not be present (ts 2000 < 2500)
    QVERIFY( !strContains( json2,  "\"20\"" ) );
  }

  void pushAndQuery_emptyStore()
  {
    MetricsHistoryStore store;
    std::string json = store.querySinceJSON( 0 );
    QCOMPARE( json, std::string( "{}" ) );
  }

  void pushAndQuery_multipleMetrics()
  {
    MetricsHistoryStore store;
    store.push( "cpuTemp", 1000, 50.0 );
    store.push( "gpuTemp", 1000, 60.0 );

    std::string json = store.querySinceJSON( 0 );
    QVERIFY( strContains( json,  "cpuTemp" ) );
    QVERIFY( strContains( json,  "gpuTemp" ) );
  }

  // ---- activeKeys() ----------------------------------------------------

  void activeKeys_returnsAllPushed()
  {
    MetricsHistoryStore store;
    store.push( "cpuTemp", 1000, 50.0 );
    store.push( "gpuTemp", 1000, 60.0 );
    store.push( "fan:hwmon3_fan1", 1000, 1200.0 );

    auto keys = store.activeKeys();
    QCOMPARE( static_cast< int >( keys.size() ), 3 );
    QVERIFY( std::find( keys.begin(), keys.end(), "cpuTemp" ) != keys.end() );
    QVERIFY( std::find( keys.begin(), keys.end(), "gpuTemp" ) != keys.end() );
    QVERIFY( std::find( keys.begin(), keys.end(), "fan:hwmon3_fan1" ) != keys.end() );
  }

  // ---- querySinceBinary() basic sanity ---------------------------------

  void binaryQuery_roundTrip()
  {
    MetricsHistoryStore store;
    store.push( "cpuTemp", 1000, 45.0 );

    auto blob = store.querySinceBinary( 0 );

    // New format: uint16_t keyLen + key chars + uint32_t count + count × 16
    // "cpuTemp" = 7 chars → 2 + 7 + 4 + 16 = 29 bytes minimum
    QVERIFY( blob.size() >= 29 );

    // Read key length
    uint16_t keyLen = 0;
    std::memcpy( &keyLen, blob.data(), sizeof( keyLen ) );
    QCOMPARE( keyLen, static_cast< uint16_t >( 7 ) );

    // Read key string
    std::string key( reinterpret_cast< const char * >( blob.data() + 2 ), keyLen );
    QCOMPARE( key, std::string( "cpuTemp" ) );

    // Read count
    uint32_t count = 0;
    std::memcpy( &count, blob.data() + 2 + keyLen, sizeof( count ) );
    QCOMPARE( count, 1u );

    // Read timestamp
    int64_t ts = 0;
    std::memcpy( &ts, blob.data() + 2 + keyLen + 4, sizeof( ts ) );
    QCOMPARE( ts, static_cast< int64_t >( 1000 ) );

    // Read value
    double val = 0.0;
    std::memcpy( &val, blob.data() + 2 + keyLen + 4 + sizeof( ts ), sizeof( val ) );
    QCOMPARE( val, 45.0 );
  }

  // ---- setHorizon() – clamping -----------------------------------------

  void horizon_default()
  {
    MetricsHistoryStore store;
    QCOMPARE( store.horizonSeconds(), MetricsHistoryStore::DEFAULT_HORIZON_S );
  }

  void horizon_clampLow()
  {
    MetricsHistoryStore store;
    store.setHorizon( 1 );  // below MIN_HORIZON_S
    QCOMPARE( store.horizonSeconds(), MetricsHistoryStore::MIN_HORIZON_S );
  }

  void horizon_clampHigh()
  {
    MetricsHistoryStore store;
    store.setHorizon( 99999 );  // above MAX_HORIZON_S
    QCOMPARE( store.horizonSeconds(), MetricsHistoryStore::MAX_HORIZON_S );
  }

  void horizon_validValue()
  {
    MetricsHistoryStore store;
    store.setHorizon( 600 );
    QCOMPARE( store.horizonSeconds(), 600 );
  }

  // ---- eviction --------------------------------------------------------

  void eviction_oldPointsPruned()
  {
    MetricsHistoryStore store;
    store.setHorizon( 60 );  // minimum = 60 seconds = 60000 ms

    // Push a point at t=1000
    store.push( "cpuTemp", 1000, 40.0 );
    // Push a point at t=200000 (well beyond 60s horizon from first point)
    store.push( "cpuTemp", 200000, 50.0 );

    // Query from the beginning — old point should have been evicted
    std::string json = store.querySinceJSON( 0 );
    // Only the 50.0 point should remain
    QVERIFY( strContains( json,  "50" ) );

    // Binary query: read header then check count = 1
    auto blob = store.querySinceBinary( 0 );
    uint16_t keyLen = 0;
    std::memcpy( &keyLen, blob.data(), sizeof( keyLen ) );
    uint32_t count = 0;
    std::memcpy( &count, blob.data() + 2 + keyLen, sizeof( count ) );
    QCOMPARE( count, 1u );
  }
};

QTEST_GUILESS_MAIN( TestMetricsHistory )

#include "test_metrics_history.moc"
