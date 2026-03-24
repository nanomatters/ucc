/*
 * Unit tests for SysfsNode – availability checks, read/write behavior,
 * delimiter/range parsing, and invalid-input handling.
 */

#include <QTemporaryDir>
#include <QTest>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "SysfsNode.hpp"

class TestSysfsNode : public QObject
{
  Q_OBJECT

private:
  QTemporaryDir m_tempDir;

  std::filesystem::path makePath( const std::string &name ) const
  {
    return std::filesystem::path( m_tempDir.path().toStdString() ) / name;
  }

  static void writeRawFile( const std::filesystem::path &path, const std::string &content )
  {
    std::ofstream out( path );
    QVERIFY( out.is_open() );
    out << content;
    QVERIFY( not out.fail() );
  }

  static std::string readRawFile( const std::filesystem::path &path )
  {
    std::ifstream in( path );
    if ( not in.is_open() )
      return {};

    return std::string( std::istreambuf_iterator< char >( in ), std::istreambuf_iterator< char >() );
  }

private slots:
  void initTestCase()
  {
    QVERIFY2( m_tempDir.isValid(), "Failed to create temporary directory" );
  }

  void availability_missing_returnsFalse()
  {
    auto path = makePath( "missing_node" );
    SysfsNode< int32_t > node( path.string() );

    QVERIFY( !node.isAvailable() );
  }

  void availability_existing_returnsTrue()
  {
    auto path = makePath( "existing_node" );
    writeRawFile( path, "42" );

    SysfsNode< int32_t > node( path.string() );
    QVERIFY( node.isAvailable() );
  }

  void readWrite_bool_roundTrip()
  {
    auto path = makePath( "bool_node" );
    writeRawFile( path, "0" );

    SysfsNode< bool > node( path.string() );

    auto initial = node.read();
    QVERIFY( initial.has_value() );
    QVERIFY( !*initial );

    QVERIFY( node.write( true ) );
    QCOMPARE( readRawFile( path ), std::string( "1" ) );

    auto updated = node.read();
    QVERIFY( updated.has_value() );
    QVERIFY( *updated );
  }

  void read_int32_roundTrip()
  {
    auto path = makePath( "int32_node" );
    writeRawFile( path, "-123" );

    SysfsNode< int32_t > node( path.string() );

    auto value = node.read();
    QVERIFY( value.has_value() );
    QCOMPARE( *value, static_cast< int32_t >( -123 ) );

    QVERIFY( node.write( 2048 ) );
    QCOMPARE( readRawFile( path ), std::string( "2048" ) );
  }

  void read_int64_roundTrip()
  {
    auto path = makePath( "int64_node" );
    writeRawFile( path, "922337203685477" );

    SysfsNode< int64_t > node( path.string() );

    auto value = node.read();
    QVERIFY( value.has_value() );
    QCOMPARE( *value, static_cast< int64_t >( 922337203685477 ) );

    QVERIFY( node.write( static_cast< int64_t >( -9000000000000 ) ) );
    QCOMPARE( readRawFile( path ), std::string( "-9000000000000" ) );
  }

  void readWrite_string_roundTrip()
  {
    auto path = makePath( "string_node" );
    writeRawFile( path, "silent" );

    SysfsNode< std::string > node( path.string() );

    auto value = node.read();
    QVERIFY( value.has_value() );
    QCOMPARE( *value, std::string( "silent" ) );

    QVERIFY( node.write( std::string( "performance" ) ) );
    QCOMPARE( readRawFile( path ), std::string( "performance" ) );
  }

  void read_vectorInt_parsesRangesAndList()
  {
    auto path = makePath( "vecint_node" );
    writeRawFile( path, "0,2,4-6" );

    SysfsNode< std::vector< int32_t > > node( path.string() );
    auto value = node.read();

    QVERIFY( value.has_value() );
    QCOMPARE( value->size(), static_cast< size_t >( 5 ) );
    QCOMPARE( value->at( 0 ), 0 );
    QCOMPARE( value->at( 1 ), 2 );
    QCOMPARE( value->at( 2 ), 4 );
    QCOMPARE( value->at( 3 ), 5 );
    QCOMPARE( value->at( 4 ), 6 );
  }

  void read_vectorInt_customDelimiter()
  {
    auto path = makePath( "vecint_node_custom" );
    writeRawFile( path, "1;3-4;8" );

    SysfsNode< std::vector< int32_t > > node( path.string(), ";" );
    auto value = node.read();

    QVERIFY( value.has_value() );
    QCOMPARE( *value, std::vector< int32_t >( {1, 3, 4, 8} ) );
  }

  void read_vectorInt_invalid_returnsNullopt()
  {
    auto path = makePath( "vecint_node_invalid" );
    writeRawFile( path, "1,abc,3" );

    SysfsNode< std::vector< int32_t > > node( path.string() );
    QVERIFY( !node.read().has_value() );
  }

  void write_vectorInt_usesDelimiter()
  {
    auto path = makePath( "vecint_write_node" );
    writeRawFile( path, "" );

    SysfsNode< std::vector< int32_t > > node( path.string(), ";" );
    QVERIFY( node.write( {10, 20, 30} ) );

    QCOMPARE( readRawFile( path ), std::string( "10;20;30" ) );
  }

  void read_vectorString_defaultAndCustomDelimiter()
  {
    {
      auto path = makePath( "vecstr_default" );
      writeRawFile( path, "cpu gpu   soc" );

      SysfsNode< std::vector< std::string > > node( path.string() );
      auto value = node.read();

      QVERIFY( value.has_value() );
      QCOMPARE( *value, std::vector< std::string >( {"cpu", "gpu", "soc"} ) );
    }

    {
      auto path = makePath( "vecstr_custom" );
      writeRawFile( path, "cpu, gpu , soc" );

      SysfsNode< std::vector< std::string > > node( path.string(), "," );
      auto value = node.read();

      QVERIFY( value.has_value() );
      QCOMPARE( *value, std::vector< std::string >( {"cpu", "gpu", "soc"} ) );
    }
  }

  void write_vectorString_usesDelimiter()
  {
    auto path = makePath( "vecstr_write" );
    writeRawFile( path, "" );

    SysfsNode< std::vector< std::string > > node( path.string(), "," );
    QVERIFY( node.write( {"zone0", "zone1", "zone2"} ) );

    QCOMPARE( readRawFile( path ), std::string( "zone0,zone1,zone2" ) );
  }

  void missingFile_readWriteFailGracefully()
  {
    auto path = makePath( "missing_parent" ) / "missing_rw";
    SysfsNode< int32_t > node( path.string() );

    QVERIFY( !node.read().has_value() );
    QVERIFY( !node.write( 7 ) );
  }
};

QTEST_GUILESS_MAIN( TestSysfsNode )

#include "test_sysfs_node.moc"
