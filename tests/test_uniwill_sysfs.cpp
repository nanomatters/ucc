/*
 * Unit tests for uniwill-laptop sysfs discovery and adapters.
 */

#include <QTemporaryDir>
#include <QTest>

#include "SysfsNode.hpp"
#include "UniwillSysfs.hpp"

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

  void profileTranslationKeepsUccProfilesCompatible()
  {
    const std::vector< std::string > uniwillProfiles = { "quiet", "balanced", "performance" };
    const std::vector< std::string > reducedProfiles = { "quiet", "balanced" };

    QCOMPARE( ucc::uniwill::translatePlatformProfileName( "power_save", uniwillProfiles ),
              std::string( "quiet" ) );
    QCOMPARE( ucc::uniwill::translatePlatformProfileName( "enthusiast", uniwillProfiles ),
              std::string( "balanced" ) );
    QCOMPARE( ucc::uniwill::translatePlatformProfileName( "overboost", uniwillProfiles ),
              std::string( "performance" ) );
    QCOMPARE( ucc::uniwill::translatePlatformProfileName( "overboost", reducedProfiles ),
              std::string( "balanced" ) );
    QCOMPARE( ucc::uniwill::translatePlatformProfileName( "balanced", uniwillProfiles ),
              std::string( "balanced" ) );
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
