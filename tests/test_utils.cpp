/*
 * Unit tests for shared utility helpers.
 */

#include <QTemporaryDir>
#include <QTest>

#include "Utils.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

class TestUtils : public QObject
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
  void deviceSupportAcceptsWhitelistedSku()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const fs::path root = rootPath( dir );

    writeFile( root / "product_sku", "XNE16E25\n" );

    QVERIFY( ucc::isDeviceSupported( root ) );
  }

  void deviceSupportAcceptsWhitelistedBaseboard()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const fs::path root = rootPath( dir );

    writeFile( root / "product_sku", "0001\n" );
    writeFile( root / "board_vendor", "AiStone\n" );
    writeFile( root / "board_name", "X6FR559Y\n" );

    QVERIFY( ucc::isDeviceSupported( root ) );
  }

  void deviceSupportRejectsUnknownBoard()
  {
    QTemporaryDir dir;
    QVERIFY( dir.isValid() );
    const fs::path root = rootPath( dir );

    writeFile( root / "product_sku", "0001\n" );
    writeFile( root / "board_vendor", "UnknownVendor\n" );
    writeFile( root / "board_name", "X6FR559Y\n" );

    QVERIFY( !ucc::isDeviceSupported( root ) );
  }
};

QTEST_GUILESS_MAIN( TestUtils )

#include "test_utils.moc"
