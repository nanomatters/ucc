/*
 * Unit tests for FanProfile - isValid(), getSpeedForTemp(),
 * getWaterCoolerFanSpeedForTemp(), getPumpSpeedForTemp()
 */

#include <QTest>
#include "profiles/FanProfile.hpp"

class TestFanProfile : public QObject
{
  Q_OBJECT

private:
  FanProfile makeSimple() const
  {
    // CPU: 30 deg->20% 50 deg->40% 70 deg->60% 90 deg->100%
    // GPU: 30 deg->25% 50 deg->45% 70 deg->65% 90 deg->100%
    return FanProfile(
      "test", "Test",
      { { 30, 20 }, { 50, 40 }, { 70, 60 }, { 90, 100 } },
      { { 30, 25 }, { 50, 45 }, { 70, 65 }, { 90, 100 } } );
  }

private slots:

  // isValid()

  void isValid_bothTables()
  {
    auto fp = makeSimple();
    QVERIFY( fp.isValid() );
  }

  void isValid_emptyCPU()
  {
    FanProfile fp( "a", "A", {}, { { 30, 20 } } );
    QVERIFY( !fp.isValid() );
  }

  void isValid_emptyGPU()
  {
    FanProfile fp( "a", "A", { { 30, 20 } }, {} );
    QVERIFY( !fp.isValid() );
  }

  void isValid_bothEmpty()
  {
    FanProfile fp( "a", "A" );
    QVERIFY( !fp.isValid() );
  }

  // getSpeedForTemp() - CPU table

  void speed_emptyTable()
  {
    FanProfile fp;
    QCOMPARE( fp.getSpeedForTemp( 50, true ), -1 );
  }

  void speed_belowFirst()
  {
    auto fp = makeSimple();
    // 10 deg is below 30 deg -> clamp to first entry speed (20%)
    QCOMPARE( fp.getSpeedForTemp( 10, true ), 20 );
  }

  void speed_exactFirst()
  {
    auto fp = makeSimple();
    QCOMPARE( fp.getSpeedForTemp( 30, true ), 20 );
  }

  void speed_exactMid()
  {
    auto fp = makeSimple();
    QCOMPARE( fp.getSpeedForTemp( 50, true ), 40 );
  }

  void speed_exactLast()
  {
    auto fp = makeSimple();
    QCOMPARE( fp.getSpeedForTemp( 90, true ), 100 );
  }

  void speed_beyondLast()
  {
    auto fp = makeSimple();
    // 100 deg beyond 90 deg -> last entry speed (100%)
    QCOMPARE( fp.getSpeedForTemp( 100, true ), 100 );
  }

  void speed_interpolateMidpoint()
  {
    auto fp = makeSimple();
    // 40 deg is midpoint of 30 deg->20% and 50 deg->40% -> lerp = 30%
    QCOMPARE( fp.getSpeedForTemp( 40, true ), 30 );
  }

  void speed_interpolateQuarter()
  {
    auto fp = makeSimple();
    // 35 deg = 1/4 of [30,50], speed = 20 + 0.25*(40-20) = 25
    QCOMPARE( fp.getSpeedForTemp( 35, true ), 25 );
  }

  void speed_gpu()
  {
    auto fp = makeSimple();
    QCOMPARE( fp.getSpeedForTemp( 50, false ), 45 );
  }

  void speed_gpuInterpolate()
  {
    auto fp = makeSimple();
    // 60 deg midpoint of 50 deg->45% and 70 deg->65% -> 55%
    QCOMPARE( fp.getSpeedForTemp( 60, false ), 55 );
  }

  // getWaterCoolerFanSpeedForTemp()

  void wcFan_fallbackToMaxCpuGpu()
  {
    auto fp = makeSimple();
    // No water cooler table -> max(CPU, GPU) at 50 deg
    // CPU=40, GPU=45 -> 45
    QCOMPARE( fp.getWaterCoolerFanSpeedForTemp( 50 ), 45 );
  }

  void wcFan_ownTable()
  {
    auto fp = makeSimple();
    fp.tableWaterCoolerFan = { { 30, 10 }, { 70, 50 } };
    // Use its own table - exact match
    QCOMPARE( fp.getWaterCoolerFanSpeedForTemp( 30 ), 10 );
  }

  void wcFan_interpolate()
  {
    auto fp = makeSimple();
    fp.tableWaterCoolerFan = { { 30, 10 }, { 70, 50 } };
    // 50 deg -> midpoint: 10 + 0.5*40 = 30
    QCOMPARE( fp.getWaterCoolerFanSpeedForTemp( 50 ), 30 );
  }

  // getPumpSpeedForTemp() - step-wise lookup

  void pump_emptyTable()
  {
    FanProfile fp;
    QCOMPARE( fp.getPumpSpeedForTemp( 50 ), ucc::PumpVoltage::Off );
  }

  void pump_belowFirst()
  {
    auto fp = makeSimple();
    fp.tablePump = { { 40, 1 }, { 60, 2 }, { 80, 3 } };
    // 30 deg < 40 deg -> never entered loop body -> Off
    QCOMPARE( fp.getPumpSpeedForTemp( 30 ), ucc::PumpVoltage::Off );
  }

  void pump_exactFirst()
  {
    auto fp = makeSimple();
    fp.tablePump = { { 40, 1 }, { 60, 2 }, { 80, 3 } };
    // speed=1 -> pumpSpeedToVoltage[1] = V7
    QCOMPARE( fp.getPumpSpeedForTemp( 40 ), ucc::PumpVoltage::V7 );
  }

  void pump_midStep()
  {
    auto fp = makeSimple();
    fp.tablePump = { { 40, 1 }, { 60, 2 }, { 80, 3 } };
    // 50 deg >= 40 deg but < 60 deg -> last match is 1 -> V7
    QCOMPARE( fp.getPumpSpeedForTemp( 50 ), ucc::PumpVoltage::V7 );
  }

  void pump_lastStep()
  {
    auto fp = makeSimple();
    fp.tablePump = { { 40, 1 }, { 60, 2 }, { 80, 3 } };
    // 80 deg matches all three entries; last match speed=3 -> V11
    QCOMPARE( fp.getPumpSpeedForTemp( 80 ), ucc::PumpVoltage::V11 );
  }

  void pump_beyondLast()
  {
    auto fp = makeSimple();
    fp.tablePump = { { 40, 1 }, { 60, 2 }, { 80, 3 } };
    // 100 deg >= all -> last match speed=3 -> V11
    QCOMPARE( fp.getPumpSpeedForTemp( 100 ), ucc::PumpVoltage::V11 );
  }
};

QTEST_GUILESS_MAIN( TestFanProfile )

#include "test_fan_profile.moc"
