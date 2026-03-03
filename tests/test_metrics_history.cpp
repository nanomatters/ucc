/*
 * Unit tests for MetricsHistoryStore – push, querySinceJSON,
 * horizon clamping, eviction, and metricName().
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

  // ---- metricName() ----------------------------------------------------

  void metricName_known()
  {
    QCOMPARE( std::string( metricName( MetricId::CpuTemp ) ),
              std::string( "cpuTemp" ) );
    QCOMPARE( std::string( metricName( MetricId::GpuPower ) ),
              std::string( "gpuPower" ) );
    QCOMPARE( std::string( metricName( MetricId::GpuCoreVoltage ) ),
              std::string( "gpuCoreVoltage" ) );
  }

  void metricName_sentinel()
  {
    // Count is a sentinel — should return "unknown"
    QCOMPARE( std::string( metricName( MetricId::Count ) ),
              std::string( "unknown" ) );
  }

  void metricName_outOfRange()
  {
    auto bad = static_cast< MetricId >( 200 );
    QCOMPARE( std::string( metricName( bad ) ),
              std::string( "unknown" ) );
  }

  // ---- push + querySinceJSON() -----------------------------------------

  void pushAndQuery_basic()
  {
    MetricsHistoryStore store;
    store.push( MetricId::CpuTemp, 1000, 45.5 );
    store.push( MetricId::CpuTemp, 2000, 46.0 );

    std::string json = store.querySinceJSON( 0 );

    // Should contain both data points
    QVERIFY( strContains( json,  "cpuTemp" ) );
    QVERIFY( strContains( json,  "45.5" ) );
    QVERIFY( strContains( json,  "46" ) );
  }

  void pushAndQuery_sinceFilters()
  {
    MetricsHistoryStore store;
    store.push( MetricId::CpuTemp, 1000, 10.0 );
    store.push( MetricId::CpuTemp, 2000, 20.0 );
    store.push( MetricId::CpuTemp, 3000, 30.0 );

    // Query since ts=2000 → should include 2000 and 3000 but not 1000
    std::string json = store.querySinceJSON( 2000 );
    QVERIFY( strContains( json,  "20" ) );
    QVERIFY( strContains( json,  "30" ) );
    // Value "10" is also a substring of timestamps, so check for the specific point
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
    store.push( MetricId::CpuTemp, 1000, 50.0 );
    store.push( MetricId::GpuTemp, 1000, 60.0 );

    std::string json = store.querySinceJSON( 0 );
    QVERIFY( strContains( json,  "cpuTemp" ) );
    QVERIFY( strContains( json,  "gpuTemp" ) );
  }

  // ---- push() ignores out-of-range MetricId ----------------------------

  void push_outOfRange()
  {
    MetricsHistoryStore store;
    auto bad = static_cast< MetricId >( 200 );
    // Should silently do nothing
    store.push( bad, 1000, 99.0 );
    QCOMPARE( store.querySinceJSON( 0 ), std::string( "{}" ) );
  }

  // ---- querySinceBinary() basic sanity ---------------------------------

  void binaryQuery_roundTrip()
  {
    MetricsHistoryStore store;
    store.push( MetricId::CpuTemp, 1000, 45.0 );

    auto blob = store.querySinceBinary( 0 );
    // At minimum: 1 byte metricId + 4 bytes count + 16 bytes (1 point) = 21
    QVERIFY( blob.size() >= 21 );

    // Verify metric ID byte
    QCOMPARE( blob[0], static_cast< uint8_t >( MetricId::CpuTemp ) );

    // Verify count = 1
    uint32_t count;
    std::memcpy( &count, blob.data() + 1, sizeof( count ) );
    QCOMPARE( count, 1u );
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
    store.push( MetricId::CpuTemp, 1000, 40.0 );
    // Push a point at t=200000 (well beyond 60s horizon from first point)
    store.push( MetricId::CpuTemp, 200000, 50.0 );

    // Query from the beginning — old point should have been evicted
    std::string json = store.querySinceJSON( 0 );
    // Only the 50.0 point should remain
    // The 40.0 point (at t=1000) is older than 200000-60000=140000 cutoff
    QVERIFY( strContains( json,  "50" ) );

    // Binary query should have exactly 1 data point
    auto blob = store.querySinceBinary( 0 );
    uint32_t count;
    std::memcpy( &count, blob.data() + 1, sizeof( count ) );
    QCOMPARE( count, 1u );
  }
};

QTEST_GUILESS_MAIN( TestMetricsHistory )

#include "test_metrics_history.moc"
