/*
 * Unit tests for uniwill-laptop sysfs discovery and adapters.
 */

#include <QTemporaryDir>
#include <QTest>

#include "SysfsNode.hpp"
#include "StorageInfo.hpp"
#include "UniwillSysfs.hpp"
#include "PowerSupplyController.hpp"

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

class TestUniwillSysfs : public QObject
{
  Q_OBJECT

private:
  static void writeFile( const fs::path &path, const std::string &value )
  {
    fs::create_directories( path.parent_path() );
    std::ofstream file( path );
    file << value;
  }

  static fs::path rootPath( const QTemporaryDir &dir )
  {
    return fs::path( dir.path().toStdString() );
  }

private slots:

  void discoveryFindsUniwillRoots()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const fs::path root = rootPath( dir );

    const fs::path platformDevice = root / "bus/platform/devices/INOU0000:00";
    fs::create_directories( platformDevice );
    writeFile( root / "class/hwmon/hwmon0/name", "uniwill\n" );
    writeFile( root / "class/platform-profile/profile0/name", "uniwill-platform-profile\n" );
    writeFile( root / "class/platform-profile/profile0/profile", "balanced\n" );
    writeFile( root / "class/platform-profile/profile0/choices", "quiet balanced performance\n" );

    const auto paths = ucc::uniwill::discover( root.string() );

    QCOMPARE( paths.platformDevicePath, platformDevice.string() );
    QCOMPARE( paths.hwmonPath, ( root / "class/hwmon/hwmon0" ).string() );
    QVERIFY( paths.platformProfile.isAvailable() );
    QCOMPARE( paths.platformProfile.description, std::string( "uniwill-platform-profile" ) );
  }

  void acpiFallbackRequiresUniwillDevice()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const fs::path root = rootPath( dir );

    writeFile( root / "firmware/acpi/platform_profile", "balanced\n" );
    writeFile( root / "firmware/acpi/platform_profile_choices", "quiet balanced performance\n" );

    QVERIFY( !ucc::uniwill::findPlatformProfile( root.string() ).has_value() );

    fs::create_directories( root / "bus/platform/devices/INOU0000:00" );
    const auto sink = ucc::uniwill::findPlatformProfile( root.string() );

    QVERIFY( sink.has_value() );
    QVERIFY( sink->isAvailable() );
    QCOMPARE( sink->profilePath, ( root / "firmware/acpi/platform_profile" ).string() );
    QCOMPARE( sink->choicesPath, ( root / "firmware/acpi/platform_profile_choices" ).string() );
  }

  void tuxedoProfilePathIsIgnored()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const fs::path root = rootPath( dir );

    writeFile( root / "bus/platform/devices/tuxedo_platform_profile/platform_profile", "balanced\n" );
    writeFile( root / "bus/platform/devices/tuxedo_platform_profile/platform_profile_choices",
               "quiet balanced performance\n" );

    const auto paths = ucc::uniwill::discover( root.string() );

    QVERIFY( !paths.platformProfile.isAvailable() );
  }

  void readsDriverInfoFromPlatformDevice()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const fs::path root = rootPath( dir );
    const fs::path infoPath = root / "bus/platform/devices/INOU0000:00/info";

    writeFile( infoPath / "project_id", "0x13\n" );
    writeFile( infoPath / "module_id", "PH4TUX1\n" );
    writeFile( infoPath / "rom_id", "1.07\n" );
    writeFile( infoPath / "ec_firmware_version", "1.09\n" );

    const auto info = ucc::uniwill::readDriverInfo( root.string() );

    QVERIFY( info.isAvailable() );
    QCOMPARE( info.infoPath, infoPath.string() );
    QCOMPARE( info.projectId.value_or( -1 ), static_cast< int32_t >( 0x13 ) );
    QCOMPARE( info.moduleId, std::string( "PH4TUX1" ) );
    QCOMPARE( info.romId, std::string( "1.07" ) );
    QCOMPARE( info.ecFirmwareVersion, std::string( "1.09" ) );
  }

  void readsCpuPowerLimitsFromPlatformDevice()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const fs::path root = rootPath( dir );
    const fs::path cpuPath = root / "bus/platform/devices/INOU0000:00/cpu";

    writeFile( cpuPath / "pl1", "35\n" );
    writeFile( cpuPath / "pl1_max", "80\n" );
    writeFile( cpuPath / "pl2", "60\n" );
    writeFile( cpuPath / "pl2_max", "90\n" );
    writeFile( cpuPath / "pl4", "100\n" );
    writeFile( cpuPath / "pl4_max", "120\n" );

    const auto limits = ucc::uniwill::readCpuPowerLimits( root.string() );

    QCOMPARE( limits.size(), static_cast< size_t >( 3 ) );
    QCOMPARE( limits[ 0 ].descriptor, std::string( "pl1" ) );
    QCOMPARE( limits[ 0 ].min, static_cast< uint32_t >( 25 ) );
    QCOMPARE( limits[ 0 ].current, static_cast< uint32_t >( 35 ) );
    QCOMPARE( limits[ 0 ].max, static_cast< uint32_t >( 80 ) );
    QCOMPARE( limits[ 1 ].descriptor, std::string( "pl2" ) );
    QCOMPARE( limits[ 2 ].descriptor, std::string( "pl4" ) );
  }

  void readsCtgpInfoFromDgpuGroup()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const fs::path root = rootPath( dir );
    const fs::path dgpuPath = root / "bus/platform/devices/INOU0000:00/dgpu";

    writeFile( dgpuPath / "ctgp_offset", "15\n" );
    writeFile( dgpuPath / "ctgp_offset_max", "25\n" );
    writeFile( dgpuPath / "tgp_base", "80\n" );

    const auto info = ucc::uniwill::readCtgpInfo( root.string() );

    QVERIFY( info.isAvailable() );
    QCOMPARE( info.offsetPath, ( dgpuPath / "ctgp_offset" ).string() );
    QCOMPARE( info.currentOffset, static_cast< int32_t >( 15 ) );
    QCOMPARE( info.maxOffset, static_cast< int32_t >( 25 ) );
    QCOMPARE( info.tgpBase, static_cast< int32_t >( 80 ) );
  }

  void readsFanInfoFromUniwillHwmon()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const fs::path root = rootPath( dir );
    const fs::path hwmonPath = root / "class/hwmon/hwmon0";

    writeFile( hwmonPath / "name", "uniwill\n" );
    writeFile( hwmonPath / "fan1_input", "2400\n" );
    writeFile( hwmonPath / "fan1_label", "Main\n" );
    writeFile( hwmonPath / "temp1_input", "55000\n" );
    writeFile( hwmonPath / "pwm1", "128\n" );
    writeFile( hwmonPath / "pwm1_enable", "2\n" );

    const auto info = ucc::uniwill::readFanInfo( root.string() );

    QVERIFY( info.isAvailable() );
    QCOMPARE( info.hwmonPath, hwmonPath.string() );
    QCOMPARE( info.channels.size(), static_cast< size_t >( 1 ) );
    QCOMPARE( info.channels[ 0 ].label, std::string( "Main" ) );
    QVERIFY( info.channels[ 0 ].canWritePwm() );
    QVERIFY( info.channels[ 0 ].canWritePwmMode() );
    QVERIFY( info.channels[ 0 ].canUseManualControl() );
    QVERIFY( ucc::uniwill::fanManualControlAvailable( info ) );
    QVERIFY( ucc::uniwill::fanOffAvailable( info ) );
    QCOMPARE( ucc::uniwill::fanMinimumSpeedPercent( info ),
              ucc::uniwill::FAN_MIN_SPEED_PERCENT );
  }

  void fanCapabilitiesRequireManualControl()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const fs::path root = rootPath( dir );
    const fs::path hwmonPath = root / "class/hwmon/hwmon0";

    writeFile( hwmonPath / "name", "uniwill\n" );
    writeFile( hwmonPath / "fan1_input", "2400\n" );
    writeFile( hwmonPath / "temp1_input", "55000\n" );
    writeFile( hwmonPath / "pwm1", "128\n" );

    const auto info = ucc::uniwill::readFanInfo( root.string() );

    QVERIFY( info.isAvailable() );
    QVERIFY( info.channels[ 0 ].canWritePwm() );
    QVERIFY( !info.channels[ 0 ].canWritePwmMode() );
    QVERIFY( !info.channels[ 0 ].canUseManualControl() );
    QVERIFY( !ucc::uniwill::fanManualControlAvailable( info ) );
    QVERIFY( !ucc::uniwill::fanOffAvailable( info ) );
    QCOMPARE( ucc::uniwill::fanMinimumSpeedPercent( info ), static_cast< int32_t >( 0 ) );
  }

  void readsFanTelemetryWithHwmonUnits()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const fs::path root = rootPath( dir );
    const fs::path hwmonPath = root / "class/hwmon/hwmon0";

    writeFile( hwmonPath / "name", "uniwill\n" );
    writeFile( hwmonPath / "fan1_input", "2400\n" );
    writeFile( hwmonPath / "temp1_input", "55500\n" );
    writeFile( hwmonPath / "pwm1", "128\n" );

    const auto info = ucc::uniwill::readFanInfo( root.string() );
    QVERIFY( info.isAvailable() );

    const auto reading = ucc::uniwill::readFanReading( info.channels[ 0 ] );

    QCOMPARE( reading.temperatureCelsius, static_cast< int32_t >( 56 ) );
    QCOMPARE( reading.speedPercent, static_cast< int32_t >( 50 ) );
    QCOMPARE( reading.rpm, static_cast< int32_t >( 2400 ) );
  }

  void readsReadOnlyHwmonTelemetryWithConvertedUnits()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const fs::path root = rootPath( dir );
    const fs::path hwmonPath = root / "class/hwmon/hwmon0";

    writeFile( hwmonPath / "name", "uniwill\n" );
    writeFile( hwmonPath / "temp1_input", "55500\n" );
    writeFile( hwmonPath / "temp2_input", "62000\n" );
    writeFile( hwmonPath / "temp3_input", "30150\n" );
    writeFile( hwmonPath / "temp4_input", "41000\n" );
    writeFile( hwmonPath / "power1_input", "155000000\n" );
    writeFile( hwmonPath / "power2_input", "80000000\n" );
    writeFile( hwmonPath / "power3_input", "24000000\n" );
    writeFile( hwmonPath / "curr1_input", "12500\n" );

    const auto telemetry = ucc::uniwill::readHwmonTelemetry( root.string() );

    QVERIFY( telemetry.isAvailable() );
    QCOMPARE( telemetry.hwmonPath, hwmonPath.string() );
    QCOMPARE( telemetry.cpuTemperatureCelsius.value_or( -1 ), static_cast< int32_t >( 56 ) );
    QCOMPARE( telemetry.gpuTemperatureCelsius.value_or( -1 ), static_cast< int32_t >( 62 ) );
    QCOMPARE( telemetry.batteryTemperatureCelsius.value_or( -1 ), static_cast< int32_t >( 30 ) );
    QCOMPARE( telemetry.ssdTemperatureCelsius.value_or( -1 ), static_cast< int32_t >( 41 ) );
    QCOMPARE( telemetry.systemPowerWatts.value_or( -1.0 ), 155.0 );
    QCOMPARE( telemetry.gpuPowerAllocationWatts.value_or( -1.0 ), 80.0 );
    QCOMPARE( telemetry.thermalBudgetWatts.value_or( -1.0 ), 24.0 );
    QCOMPARE( telemetry.adapterCurrentAmps.value_or( -1.0 ), 12.5 );
  }

  void readsDramTemperaturesFromSpdSensors()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const fs::path root = rootPath( dir );

    // Two DDR5 spd5118 sensors (out of slot order on disk) + a non-DRAM hwmon to ignore.
    const fs::path hwA = root / "class/hwmon/hwmon8"; // slot 1
    const fs::path hwB = root / "class/hwmon/hwmon7"; // slot 0
    const fs::path other = root / "class/hwmon/hwmon9";
    writeFile( hwA / "name", "spd5118\n" );
    writeFile( hwA / "temp1_input", "52500\n" );   // -> 53 C (lround 52.5)
    writeFile( hwB / "name", "spd5118\n" );
    writeFile( hwB / "temp1_input", "53750\n" );   // -> 54 C
    writeFile( other / "name", "uniwill\n" );
    writeFile( other / "temp1_input", "60000\n" );

    // device symlinks carry the i2c address that yields the slot index
    fs::create_directories( root / "devices/i2c/11-0050" );
    fs::create_directories( root / "devices/i2c/11-0051" );
    fs::create_directory_symlink( root / "devices/i2c/11-0051", hwA / "device" );
    fs::create_directory_symlink( root / "devices/i2c/11-0050", hwB / "device" );

    const auto temps = ucc::uniwill::readDramTemperatures( root.string() );

    QCOMPARE( temps.size(), static_cast< size_t >( 2 ) );
    // Sorted by slot derived from the SPD i2c address (0x50 -> 0, 0x51 -> 1)
    QCOMPARE( temps[ 0 ].slot, 0 );
    QCOMPARE( temps[ 0 ].temperatureCelsius, 54 );
    QCOMPARE( temps[ 1 ].slot, 1 );
    QCOMPARE( temps[ 1 ].temperatureCelsius, 53 );
  }

  void readsStorageInventoryAndTemperatures()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const fs::path root = rootPath( dir );

    const fs::path nvme0 = root / "block/nvme0n1";
    const fs::path nvme1 = root / "block/nvme1n1";
    const fs::path ignored = root / "block/zram0";
    writeFile( nvme0 / "size", "3907029168\n" );
    writeFile( nvme0 / "removable", "0\n" );
    writeFile( nvme0 / "queue/rotational", "0\n" );
    writeFile( nvme1 / "size", "1953525168\n" );
    writeFile( nvme1 / "removable", "0\n" );
    writeFile( nvme1 / "queue/rotational", "0\n" );
    writeFile( ignored / "size", "1048576\n" );
    writeFile( ignored / "removable", "0\n" );
    writeFile( ignored / "queue/rotational", "0\n" );

    fs::create_directories( root / "devices/pci/nvme/nvme0/hwmon3" );
    fs::create_directories( root / "devices/pci/nvme/nvme1/hwmon4" );
    fs::create_directory_symlink( root / "devices/pci/nvme/nvme0", nvme0 / "device" );
    fs::create_directory_symlink( root / "devices/pci/nvme/nvme1", nvme1 / "device" );
    writeFile( nvme0 / "device/model", "  Samsung   SSD 990 PRO  \n" );
    writeFile( nvme1 / "device/model", "WD_BLACK SN850X\n" );

    const fs::path hwmon0 = root / "devices/pci/nvme/nvme0/hwmon3";
    const fs::path hwmon1 = root / "devices/pci/nvme/nvme1/hwmon4";
    writeFile( hwmon0 / "name", "nvme\n" );
    writeFile( hwmon0 / "temp1_label", "Sensor 1\n" );
    writeFile( hwmon0 / "temp1_input", "60000\n" );
    writeFile( hwmon0 / "temp2_label", "Composite\n" );
    writeFile( hwmon0 / "temp2_input", "38850\n" );
    writeFile( hwmon1 / "name", "nvme\n" );
    writeFile( hwmon1 / "temp1_input", "35850\n" );
    fs::create_directories( root / "class/hwmon" );
    fs::create_directory_symlink( hwmon0, root / "class/hwmon/hwmon3" );
    fs::create_directory_symlink( hwmon1, root / "class/hwmon/hwmon4" );

    const auto devices = detectStorageDevices( root.string() );
    QCOMPARE( devices.size(), static_cast< size_t >( 2 ) );
    QCOMPARE( devices[ 0 ].name, std::string( "nvme0n1" ) );
    QCOMPARE( devices[ 0 ].model, std::string( "Samsung SSD 990 PRO" ) );
    QCOMPARE( devices[ 1 ].name, std::string( "nvme1n1" ) );
    QCOMPARE( devices[ 1 ].model, std::string( "WD_BLACK SN850X" ) );

    const auto temps = readStorageTemperatures( root.string() );
    QCOMPARE( temps.size(), static_cast< size_t >( 2 ) );
    QCOMPARE( temps[ 0 ].name, std::string( "nvme0n1" ) );
    QCOMPARE( temps[ 0 ].temperatureCelsius, 39 );
    QCOMPARE( temps[ 1 ].name, std::string( "nvme1n1" ) );
    QCOMPARE( temps[ 1 ].temperatureCelsius, 36 );
  }

  void writesFanManualPwmAndMode()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const fs::path root = rootPath( dir );
    const fs::path hwmonPath = root / "class/hwmon/hwmon0";

    writeFile( hwmonPath / "name", "uniwill\n" );
    writeFile( hwmonPath / "fan1_input", "2400\n" );
    writeFile( hwmonPath / "temp1_input", "55000\n" );
    writeFile( hwmonPath / "pwm1", "0\n" );
    writeFile( hwmonPath / "pwm1_enable", "2\n" );

    const auto info = ucc::uniwill::readFanInfo( root.string() );
    QVERIFY( info.isAvailable() );

    QVERIFY( ucc::uniwill::writeFanMode( info, 1 ) );
    QVERIFY( ucc::uniwill::writeFanPwm( info.channels[ 0 ], 80 ) );

    QCOMPARE( ucc::uniwill::readInt32( hwmonPath / "pwm1_enable" ).value_or( -1 ),
              static_cast< int32_t >( 1 ) );
    QCOMPARE( ucc::uniwill::readInt32( hwmonPath / "pwm1" ).value_or( -1 ),
              static_cast< int32_t >( 204 ) );
  }

  void readsAndWritesUsbCPowerPriority()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const fs::path root = rootPath( dir );
    const fs::path devicePath = root / "bus/platform/devices/INOU0000:00";

    writeFile( devicePath / "usb_c_power_priority", "charging\n" );

    const auto priority = ucc::uniwill::readUsbCPowerPriority( root.string() );

    QVERIFY( priority.isAvailable() );
    QCOMPARE( priority.current, std::string( "charging" ) );
    QCOMPARE( priority.choices.size(), static_cast< size_t >( 2 ) );
    QVERIFY( ucc::uniwill::contains( priority.choices, "performance" ) );

    QVERIFY( ucc::uniwill::writeUsbCPowerPriority( priority, "performance" ) );
    QCOMPARE( ucc::uniwill::readFirstLine( devicePath / "usb_c_power_priority" ).value_or( "" ),
              std::string( "performance" ) );
    QVERIFY( ucc::uniwill::writeUsbCPowerPriority( priority, "charge_battery" ) );
    QCOMPARE( ucc::uniwill::readFirstLine( devicePath / "usb_c_power_priority" ).value_or( "" ),
              std::string( "charging" ) );
    QVERIFY( !ucc::uniwill::writeUsbCPowerPriority( priority, "invalid" ) );
  }

  void readsAndWritesWaterCoolerBridgeEnable()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const fs::path root = rootPath( dir );
    const fs::path devicePath = root / "bus/platform/devices/INOU0000:00";

    writeFile( devicePath / "wc/enable", "0\n" );

    auto bridge = ucc::uniwill::readWaterCoolerBridge( root.string() );

    QVERIFY( bridge.isAvailable() );
    QCOMPARE( bridge.enablePath, ( devicePath / "wc/enable" ).string() );
    QVERIFY( !bridge.enabled );

    QVERIFY( ucc::uniwill::writeWaterCoolerBridgeEnable( bridge, true ) );
    bridge = ucc::uniwill::readWaterCoolerBridge( root.string() );
    QVERIFY( bridge.enabled );

    QVERIFY( ucc::uniwill::writeWaterCoolerBridgeEnable( false, root.string() ) );
    bridge = ucc::uniwill::readWaterCoolerBridge( root.string() );
    QVERIFY( !bridge.enabled );
  }

  void readsDgpuPlatformState()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const fs::path root = rootPath( dir );
    const fs::path devicePath = root / "bus/platform/devices/INOU0000:00";

    writeFile( devicePath / "dgpu/tgp_base", "80\n" );
    writeFile( devicePath / "dgpu/ctgp_offset", "15\n" );
    writeFile( devicePath / "dgpu/db_offset_max", "20\n" );
    writeFile( devicePath / "dgpu/dynamic_boost_enable", "1\n" );
    writeFile( devicePath / "dgpu/mux_mode", "hybrid\n" );

    const auto state = ucc::uniwill::readDgpuPlatformState( root.string() );

    QVERIFY( state.isAvailable() );
    QCOMPARE( state.tgpBase.value_or( -1 ), static_cast< int32_t >( 80 ) );
    QCOMPARE( state.currentCtgpOffset.value_or( -1 ), static_cast< int32_t >( 15 ) );
    QCOMPARE( state.maxDynamicBoostOffset.value_or( -1 ), static_cast< int32_t >( 20 ) );
    QVERIFY( state.dynamicBoostEnabled.value_or( false ) );
    QCOMPARE( state.muxMode, std::string( "hybrid" ) );
  }

  void writesDgpuDynamicBoostEnable()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const fs::path root = rootPath( dir );
    const fs::path enablePath =
      root / "bus/platform/devices/INOU0000:00/dgpu/dynamic_boost_enable";

    writeFile( enablePath, "1\n" );

    const auto control = ucc::uniwill::readDgpuDynamicBoostControl( root.string() );
    QVERIFY( control.isAvailable() );
    QVERIFY( control.enabled.value_or( false ) );

    const SysfsWriteResult result =
      ucc::uniwill::writeDgpuDynamicBoostEnable( control, false );

    QVERIFY( result );

    const auto updated = ucc::uniwill::readDgpuDynamicBoostControl( root.string() );
    QVERIFY( updated.enabled.has_value() );
    QVERIFY( !*updated.enabled );
  }

  void readsAndWritesDgpuMuxMode()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const fs::path root = rootPath( dir );
    const fs::path modePath =
      root / "bus/platform/devices/INOU0000:00/dgpu/mux_mode";

    writeFile( modePath, "hybrid\n" );

    auto control = ucc::uniwill::readDgpuMuxControl( root.string() );
    QVERIFY( control.isAvailable() );
    QCOMPARE( control.modePath, modePath.string() );
    QCOMPARE( control.mode, std::string( "hybrid" ) );

    const SysfsWriteResult result =
      ucc::uniwill::writeDgpuMuxMode( control, "dgpu_direct" );

    QVERIFY( result );
    QCOMPARE( ucc::uniwill::readFirstLine( modePath ).value_or( "" ),
              std::string( "dgpu_direct" ) );

    control = ucc::uniwill::readDgpuMuxControl( root.string() );
    QCOMPARE( control.mode, std::string( "dgpu_direct" ) );

    const SysfsWriteResult invalid =
      ucc::uniwill::writeDgpuMuxMode( control, "discrete" );
    QVERIFY( !invalid );
    QCOMPARE( invalid.error, EINVAL );
  }

  void chargeEndThresholdAvailabilityRequiresWritableNode()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const fs::path batteryPath = rootPath( dir ) / "BAT0";

    writeFile( batteryPath / "charge_control_end_threshold", "80\n" );
    fs::permissions(
      batteryPath / "charge_control_end_threshold",
      fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read,
      fs::perm_options::replace );

    PowerSupplyController battery( batteryPath.string() );
    QVERIFY( battery.hasChargeControlEndThreshold() );
    QVERIFY( !battery.isChargeControlEndThresholdWritable() );
    QVERIFY( battery.getChargeControlEndAvailableThresholds().empty() );

    fs::permissions(
      batteryPath / "charge_control_end_threshold",
      fs::perms::owner_read | fs::perms::owner_write,
      fs::perm_options::replace );

    const auto thresholds = battery.getChargeControlEndAvailableThresholds();
    QCOMPARE( thresholds.size(), static_cast< size_t >( 100 ) );
    QCOMPARE( thresholds.front(), 1 );
    QCOMPARE( thresholds.back(), 100 );
  }

  void readsBatteryHealth()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const fs::path batteryPath = rootPath( dir ) / "BAT0";

    writeFile( batteryPath / "health", "Unspecified failure\n" );

    const PowerSupplyController battery( batteryPath.string() );
    QCOMPARE( battery.getHealth(), std::string( "Unspecified failure" ) );
  }

  void sysfsWriteDetailedReportsErrno()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const fs::path missingNode = rootPath( dir ) / "missing_sysfs_node";

    const SysfsWriteResult result = SysfsNode< int64_t >( missingNode.string() ).writeDetailed( 42 );

    QVERIFY( !result );
    QCOMPARE( result.error, ENOENT );
    QCOMPARE( result.bytesWritten, static_cast< size_t >( 0 ) );
  }

  void sysfsWriteDetailedReportsPermissionDenied()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const fs::path readOnlyNode = rootPath( dir ) / "readonly_sysfs_node";

    writeFile( readOnlyNode, "1\n" );
    fs::permissions(
      readOnlyNode,
      fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read,
      fs::perm_options::replace );

    const SysfsWriteResult result = SysfsNode< int64_t >( readOnlyNode.string() ).writeDetailed( 42 );

    fs::permissions(
      readOnlyNode,
      fs::perms::owner_read | fs::perms::owner_write,
      fs::perm_options::replace );

    QVERIFY( !result );
    QCOMPARE( result.error, EACCES );
    QCOMPARE( result.bytesWritten, static_cast< size_t >( 0 ) );
  }
};

QTEST_GUILESS_MAIN( TestUniwillSysfs )

#include "test_uniwill_sysfs.moc"
