/*
 * Unit tests for ProfileManager – parseProfileJSON() / profileToJSON()
 * round-trip, and the low-level extract helpers.
 */

#include <QTest>
#include "ProfileManager.hpp"

class TestProfileManager : public QObject
{
  Q_OBJECT

private:
  /// Minimal valid profile JSON for round-trip tests
  static std::string minimalJSON()
  {
    return R"({
      "id":   "test-profile-42",
      "name": "My Test Profile",
      "description": "A unit-test profile",
      "display": {
        "brightness": 80,
        "useBrightness": true,
        "refreshRate": 144,
        "useRefRate": true,
        "xResolution": 1920,
        "yResolution": 1080,
        "useResolution": false
      },
      "cpu": {
        "onlineCores": 8,
        "scalingMinFrequency": 800000,
        "scalingMaxFrequency": 4500000,
        "governor": "powersave",
        "energyPerformancePreference": "balance_performance",
        "noTurbo": false
      },
      "webcam": { "status": true, "useStatus": false },
      "fan": {
        "useControl": true,
        "fanProfile": "fan-balanced",
        "sameSpeed": false,
        "autoControlWC": true,
        "enableWaterCooler": false,
        "tableCPU": [{"temp":30,"speed":20},{"temp":70,"speed":60}],
        "tableGPU": [{"temp":30,"speed":25},{"temp":70,"speed":65}]
      },
      "odmProfile": { "name": "enthusiast" },
      "odmPowerLimits": { "tdpValues": [45, 80] },
      "keyboard": { "keyboardProfileName": "Rainbow" },
      "selectedKeyboardProfile": "kb-uuid-001",
      "chargingProfile": "balanced",
      "chargingPriority": "performance",
      "chargeType": "Standard",
      "chargeStartThreshold": 40,
      "chargeEndThreshold": 80
    })";
  }

private slots:

  // ---- parseProfileJSON() – field extraction ---------------------------

  void parseProfile_idName()
  {
    auto p = ProfileManager::parseProfileJSON( minimalJSON() );
    QCOMPARE( p.id,          std::string( "test-profile-42" ) );
    QCOMPARE( p.name,        std::string( "My Test Profile" ) );
    QCOMPARE( p.description, std::string( "A unit-test profile" ) );
  }

  void parseProfile_display()
  {
    auto p = ProfileManager::parseProfileJSON( minimalJSON() );
    QCOMPARE( p.display.brightness, 80 );
    QVERIFY( p.display.useBrightness );
    QCOMPARE( p.display.refreshRate, 144 );
    QVERIFY( p.display.useRefRate );
    QCOMPARE( p.display.xResolution, 1920 );
    QCOMPARE( p.display.yResolution, 1080 );
    QVERIFY( !p.display.useResolution );
  }

  void parseProfile_cpu()
  {
    auto p = ProfileManager::parseProfileJSON( minimalJSON() );
    QVERIFY( p.cpu.onlineCores.has_value() );
    QCOMPARE( *p.cpu.onlineCores, 8 );
    QVERIFY( p.cpu.scalingMinFrequency.has_value() );
    QCOMPARE( *p.cpu.scalingMinFrequency, 800000 );
    QVERIFY( p.cpu.scalingMaxFrequency.has_value() );
    QCOMPARE( *p.cpu.scalingMaxFrequency, 4500000 );
    QCOMPARE( p.cpu.governor, std::string( "powersave" ) );
    QCOMPARE( p.cpu.energyPerformancePreference, std::string( "balance_performance" ) );
    QVERIFY( !p.cpu.noTurbo );
  }

  void parseProfile_fan()
  {
    auto p = ProfileManager::parseProfileJSON( minimalJSON() );
    QVERIFY( p.fan.useControl );
    QCOMPARE( p.fan.fanProfile, std::string( "fan-balanced" ) );
    QVERIFY( !p.fan.sameSpeed );
    QVERIFY( p.fan.autoControlWC );
    QVERIFY( !p.fan.enableWaterCooler );
  }

  void parseProfile_embeddedFanTables()
  {
    auto p = ProfileManager::parseProfileJSON( minimalJSON() );
    QCOMPARE( static_cast< int >( p.fan.tableCPU.size() ), 2 );
    QCOMPARE( p.fan.tableCPU[0].temp, 30 );
    QCOMPARE( p.fan.tableCPU[0].speed, 20 );
    QCOMPARE( p.fan.tableCPU[1].temp, 70 );
    QCOMPARE( p.fan.tableCPU[1].speed, 60 );

    QCOMPARE( static_cast< int >( p.fan.tableGPU.size() ), 2 );
    QCOMPARE( p.fan.tableGPU[0].speed, 25 );
  }

  void parseProfile_odmAndNvidia()
  {
    auto p = ProfileManager::parseProfileJSON( minimalJSON() );
    QVERIFY( p.odmProfile.name.has_value() );
    QCOMPARE( *p.odmProfile.name, std::string( "enthusiast" ) );
    QCOMPARE( static_cast< int >( p.odmPowerLimits.tdpValues.size() ), 2 );
    QCOMPARE( p.odmPowerLimits.tdpValues[0], 45 );
    // nvidiaPowerCTRLProfile removed from general profile — lives only in GPU profile
  }

  void parseProfile_keyboard()
  {
    auto p = ProfileManager::parseProfileJSON( minimalJSON() );
    QCOMPARE( p.keyboard.keyboardProfileId,   std::string( "kb-uuid-001" ) );
    QCOMPARE( p.keyboard.keyboardProfileName, std::string( "Rainbow" ) );
  }

  void parseProfile_charging()
  {
    auto p = ProfileManager::parseProfileJSON( minimalJSON() );
    QCOMPARE( p.chargingProfile,       std::string( "balanced" ) );
    QCOMPARE( p.chargingPriority,      std::string( "performance" ) );
    QCOMPARE( p.chargeType,            std::string( "Standard" ) );
    QCOMPARE( p.chargeStartThreshold,  40 );
    QCOMPARE( p.chargeEndThreshold,    80 );
  }

  // ---- round-trip: parse → serialize → re-parse -------------------------

  void roundTrip()
  {
    auto original = ProfileManager::parseProfileJSON( minimalJSON() );
    std::string json2 = ProfileManager::profileToJSON( original );
    auto reparsed = ProfileManager::parseProfileJSON( json2 );

    QCOMPARE( reparsed.id,          original.id );
    QCOMPARE( reparsed.name,        original.name );
    QCOMPARE( reparsed.description, original.description );
    QCOMPARE( reparsed.display.brightness,    original.display.brightness );
    QCOMPARE( reparsed.display.useBrightness, original.display.useBrightness );
    QCOMPARE( reparsed.cpu.governor,          original.cpu.governor );
    QCOMPARE( reparsed.fan.fanProfile,        original.fan.fanProfile );
    QCOMPARE( reparsed.fan.sameSpeed,         original.fan.sameSpeed );
    QCOMPARE( reparsed.chargingProfile,       original.chargingProfile );
    QCOMPARE( reparsed.chargeStartThreshold,  original.chargeStartThreshold );
    QCOMPARE( reparsed.chargeEndThreshold,    original.chargeEndThreshold );

    // Fan tables survive the trip
    QCOMPARE( static_cast< int >( reparsed.fan.tableCPU.size() ),
              static_cast< int >( original.fan.tableCPU.size() ) );
    for ( size_t i = 0; i < original.fan.tableCPU.size(); ++i )
    {
      QCOMPARE( reparsed.fan.tableCPU[i].temp,  original.fan.tableCPU[i].temp );
      QCOMPARE( reparsed.fan.tableCPU[i].speed, original.fan.tableCPU[i].speed );
    }
  }

  // ---- parseFanTableFromJSON() -----------------------------------------

  void parseFanTable_valid()
  {
    // parseFanTableFromJSON expects a bare JSON array of {temp, speed} objects
    std::string json = R"([{"temp":30,"speed":20},{"temp":50,"speed":40}])";
    auto table = ProfileManager::parseFanTableFromJSON( json );
    QCOMPARE( static_cast< int >( table.size() ), 2 );
    QCOMPARE( table[0].temp, 30 );
    QCOMPARE( table[1].speed, 40 );
  }

  void parseFanTable_empty()
  {
    std::string json = R"([])";
    auto table = ProfileManager::parseFanTableFromJSON( json );
    QVERIFY( table.empty() );
  }
};

QTEST_GUILESS_MAIN( TestProfileManager )

#include "test_profile_manager.moc"
