/*
 * Unit tests for FanProfile – isValid(), findZoneCurve(), getSpeedForZone(),
 * interpolateCurve(), and zone lookup behavior.
 */

#include <QTest>
#include "profiles/FanProfile.hpp"

using ucc::hal::FanCurvePoint;
using ucc::hal::FanZoneCurve;

class TestFanProfile : public QObject
{
  Q_OBJECT

private:
  static FanZoneCurve makeCurve( const std::string &zoneId,
                                 std::vector< FanCurvePoint > curve )
  {
    return FanZoneCurve( zoneId, std::move( curve ) );
  }

  FanProfile makeSimple() const
  {
    // CPU zone:  30°→20%  50°→40%  70°→60%  90°→100%
    // GPU zone:  30°→25%  50°→45%  70°→65%  90°→100%
    return FanProfile(
      "test", "Test",
      {
        makeCurve( "zone-cpu",
          { {30,20}, {50,40}, {70,60}, {90,100} } ),
        makeCurve( "zone-gpu",
          { {30,25}, {50,45}, {70,65}, {90,100} } ),
      } );
  }

private slots:

  // ---- isValid() -------------------------------------------------------

  void isValid_withZones()
  {
    auto fp = makeSimple();
    QVERIFY( fp.isValid() );
  }

  void isValid_noZones()
  {
    FanProfile fp( "a", "A" );
    QVERIFY( !fp.isValid() );
  }

  // ---- findZoneCurve() ------------------------------------------------------

  void findZoneCurve_exists()
  {
    auto fp = makeSimple();
    QVERIFY( fp.findZoneCurve( "zone-cpu" ) != nullptr );
    QCOMPARE( fp.findZoneCurve( "zone-cpu" )->zoneId, std::string( "zone-cpu" ) );
  }

  void findZoneCurve_missing()
  {
    auto fp = makeSimple();
    QVERIFY( fp.findZoneCurve( "zone-nonexistent" ) == nullptr );
  }

  // ---- getSpeedForZone() – CPU zone ------------------------------------

  void speed_emptyProfile()
  {
    FanProfile fp;
    QCOMPARE( fp.getSpeedForZone( 50, "zone-cpu" ), -1 );
  }

  void speed_missingZone()
  {
    auto fp = makeSimple();
    QCOMPARE( fp.getSpeedForZone( 50, "zone-nonexistent" ), -1 );
  }

  void speed_belowFirst()
  {
    auto fp = makeSimple();
    // 10° below 30° → clamp to first entry speed (20%)
    QCOMPARE( fp.getSpeedForZone( 10, "zone-cpu" ), 20 );
  }

  void speed_exactFirst()
  {
    auto fp = makeSimple();
    QCOMPARE( fp.getSpeedForZone( 30, "zone-cpu" ), 20 );
  }

  void speed_exactMid()
  {
    auto fp = makeSimple();
    QCOMPARE( fp.getSpeedForZone( 50, "zone-cpu" ), 40 );
  }

  void speed_exactLast()
  {
    auto fp = makeSimple();
    QCOMPARE( fp.getSpeedForZone( 90, "zone-cpu" ), 100 );
  }

  void speed_beyondLast()
  {
    auto fp = makeSimple();
    // 100° beyond 90° → last entry speed (100%)
    QCOMPARE( fp.getSpeedForZone( 100, "zone-cpu" ), 100 );
  }

  void speed_interpolateMidpoint()
  {
    auto fp = makeSimple();
    // 40° is midpoint of 30°→20% and 50°→40%  → lerp = 30%
    QCOMPARE( fp.getSpeedForZone( 40, "zone-cpu" ), 30 );
  }

  void speed_interpolateQuarter()
  {
    auto fp = makeSimple();
    // 35° = ¼ of [30,50], speed = 20 + 0.25*(40-20) = 25
    QCOMPARE( fp.getSpeedForZone( 35, "zone-cpu" ), 25 );
  }

  // ---- getSpeedForZone() – GPU zone ------------------------------------

  void speed_gpu()
  {
    auto fp = makeSimple();
    QCOMPARE( fp.getSpeedForZone( 50, "zone-gpu" ), 45 );
  }

  void speed_gpuInterpolate()
  {
    auto fp = makeSimple();
    // 60° midpoint of 50°→45% and 70°→65%  → 55%
    QCOMPARE( fp.getSpeedForZone( 60, "zone-gpu" ), 55 );
  }

  // ---- WC fan zone -----------------------------------------------------

  void wcFan_ownCurve()
  {
    auto fp = makeSimple();
    fp.zoneCurves.push_back( makeCurve( "wc-fan",
      { {30,10}, {70,50} } ) );
    QCOMPARE( fp.getSpeedForZone( 30, "wc-fan" ), 10 );
  }

  void wcFan_interpolate()
  {
    auto fp = makeSimple();
    fp.zoneCurves.push_back( makeCurve( "wc-fan",
      { {30,10}, {70,50} } ) );
    // 50° → midpoint: 10 + 0.5*40 = 30
    QCOMPARE( fp.getSpeedForZone( 50, "wc-fan" ), 30 );
  }

  // ---- WC pump zone (discrete voltage levels) --------------------------

  void pump_lookup()
  {
    auto fp = makeSimple();
    fp.zoneCurves.push_back( makeCurve( "wc-pump",
      { {40,1}, {60,2}, {80,3} } ) );
    // interpolateCurve uses linear, so exact matches just return the speed value
    QCOMPARE( fp.getSpeedForZone( 40, "wc-pump" ), 1 );
    QCOMPARE( fp.getSpeedForZone( 60, "wc-pump" ), 2 );
    QCOMPARE( fp.getSpeedForZone( 80, "wc-pump" ), 3 );
  }

  void pump_beyondLast()
  {
    auto fp = makeSimple();
    fp.zoneCurves.push_back( makeCurve( "wc-pump",
      { {40,1}, {60,2}, {80,3} } ) );
    // 100° beyond 80° → clamp to last (3)
    QCOMPARE( fp.getSpeedForZone( 100, "wc-pump" ), 3 );
  }

  void pump_belowFirst()
  {
    auto fp = makeSimple();
    fp.zoneCurves.push_back( makeCurve( "wc-pump",
      { {40,1}, {60,2}, {80,3} } ) );
    // 30° below 40° → clamp to first (1)
    QCOMPARE( fp.getSpeedForZone( 30, "wc-pump" ), 1 );
  }
};

QTEST_GUILESS_MAIN( TestFanProfile )

#include "test_fan_profile.moc"
