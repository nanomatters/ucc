/*
 * Unit tests for StateUtils - profileStateToString()
 */

#include <QTest>

// StateUtils.hpp pulls in TccSettings.hpp (for ProfileState) and
// PowerSupplyController.hpp. We only test the pure enum->string helper,
// so the filesystem-touching code in determineState() is never called.
#include "StateUtils.hpp"

class TestStateUtils : public QObject
{
  Q_OBJECT

private slots:

  // profileStateToString()

  void toStringAC()
  {
    QCOMPARE( profileStateToString( ProfileState::AC ), std::string( "power_ac" ) );
  }

  void toStringBAT()
  {
    QCOMPARE( profileStateToString( ProfileState::BAT ), std::string( "power_bat" ) );
  }

  void toStringWC()
  {
    QCOMPARE( profileStateToString( ProfileState::WC ), std::string( "power_wc" ) );
  }

  void toStringDefault()
  {
    // Any unknown value should fall through to "power_ac"
    auto garbage = static_cast< ProfileState >( 99 );
    QCOMPARE( profileStateToString( garbage ), std::string( "power_ac" ) );
  }
};

QTEST_GUILESS_MAIN( TestStateUtils )

#include "test_state_utils.moc"
