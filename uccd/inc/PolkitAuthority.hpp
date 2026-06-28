/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <string>
#include <syslog.h>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusInterface>
#include <QDBusUnixFileDescriptor>
#include <QVariantList>
#include <QVariantMap>

/**
 * @brief Polkit authorization checker for uccd D-Bus methods.
 *
 * Uses the org.freedesktop.PolicyKit1 D-Bus interface to verify that the
 * calling process is authorized for the requested action.
 *
 * Three authorization levels are defined:
 *
 * - com.uniwill.uccd.read (read-only queries)
 * - com.uniwill.uccd.control (profiles, backlight, fan curves, etc.)
 * - com.uniwill.uccd.manage-hardware (TDP, fan disable, charge thresholds, cTGP, pump voltage)
 */
class PolkitAuthority
{
public:
  /// Authorization action IDs
  static constexpr const char *ACTION_READ             = "com.uniwill.uccd.read";
  static constexpr const char *ACTION_CONTROL          = "com.uniwill.uccd.control";
  static constexpr const char *ACTION_MANAGE_HARDWARE  = "com.uniwill.uccd.manage-hardware";

  /**
   * @brief Check whether the D-Bus caller is authorized for a Polkit action.
   *
   * @param connection The QDBusConnection the call arrived on (system bus)
   * @param message The incoming QDBusMessage (contains the caller's service name)
   * @param actionId One of the ACTION_* constants above
   * @return true if authorized, false otherwise
   */
  static bool checkAuthorization( const QDBusConnection &connection,
                                  const QDBusMessage &message,
                                  const char *actionId ) noexcept
  {
    try
    {
      // Retrieve the caller's PID via the bus daemon
      const QString sender = message.service();

      QDBusMessage getPid = QDBusMessage::createMethodCall(
          "org.freedesktop.DBus",
          "/org/freedesktop/DBus",
          "org.freedesktop.DBus",
          "GetConnectionUnixProcessID" );
      getPid << sender;

      QDBusReply< uint > pidReply = connection.call( getPid );
      if ( not pidReply.isValid() )
      {
        syslog( LOG_WARNING, "PolkitAuthority: Failed to get caller PID: %s",
                pidReply.error().message().toStdString().c_str() );
        return false;
      }
      const uint callerPid = pidReply.value();

      // Build the Polkit subject: ("unix-process", { "pid": uint32, "start-time": uint64 })
      // start-time = 0 means "don't check" (Polkit falls back to /proc/<pid>)
      QVariantMap subjectDetails;
      subjectDetails["pid"] = QVariant::fromValue( callerPid );
      subjectDetails["start-time"] = QVariant::fromValue( static_cast< quint64 >( 0 ) );

      // The Polkit CheckAuthorization call uses (sa{sv}) for the subject struct.
      // We must build the struct using QDBusArgument.
      QDBusMessage polkitCall = QDBusMessage::createMethodCall(
          "org.freedesktop.PolicyKit1",
          "/org/freedesktop/PolicyKit1/Authority",
          "org.freedesktop.PolicyKit1.Authority",
          "CheckAuthorization" );

      // Subject struct: (sa{sv})
      QDBusArgument subject;
      subject.beginStructure();
      subject << QString( "unix-process" ) << subjectDetails;
      subject.endStructure();

      // Details: a{ss} (empty map of string->string)
      QDBusArgument details;
      details.beginMap( QMetaType::fromType< QString >(), QMetaType::fromType< QString >() );
      details.endMap();

      // flags: 0x1 = AllowUserInteraction (show password prompt if needed)
      quint32 flags = 0x1;

      // cancellation_id: empty string
      QString cancellationId;

      polkitCall << QVariant::fromValue( subject )
                 << QString( actionId )
                 << QVariant::fromValue( details )
                 << flags
                 << cancellationId;

      QDBusMessage polkitReply = connection.call( polkitCall, QDBus::Block, 60000 );

      if ( polkitReply.type() == QDBusMessage::ErrorMessage )
      {
        syslog( LOG_WARNING, "PolkitAuthority: Polkit error for action '%s': %s",
                actionId, polkitReply.errorMessage().toStdString().c_str() );
        return false;
      }

      // The reply is a struct (bba{ss}): (is_authorized, is_challenge, details)
      const QVariant argVariant = polkitReply.arguments().value( 0 );
      const QDBusArgument resultArg = argVariant.value< QDBusArgument >();

      bool isAuthorized = false;
      bool isChallenge = false;
      QMap< QString, QString > resultDetails;

      resultArg.beginStructure();
      resultArg >> isAuthorized >> isChallenge >> resultDetails;
      resultArg.endStructure();

      if ( not isAuthorized )
      {
        syslog( LOG_NOTICE, "PolkitAuthority: PID %u denied for action '%s'",
                callerPid, actionId );
      }

      return isAuthorized;
    }
    catch ( const std::exception &e )
    {
      syslog( LOG_ERR, "PolkitAuthority: Exception during auth check: %s", e.what() );
      return false;
    }
    catch ( ... )
    {
      syslog( LOG_ERR, "PolkitAuthority: Unknown exception during auth check" );
      return false;
    }
  }
};
