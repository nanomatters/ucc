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

#include <cstring>
#include <string>
#include <vector>
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
 *  - com.uniwill.uccd.read            (read-only queries)
 *  - com.uniwill.uccd.control         (profiles, backlight, fan curves, etc.)
 *  - com.uniwill.uccd.manage-hardware (TDP, fan disable, charge thresholds, cTGP, pump voltage)
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
   * @param connection   The QDBusConnection the call arrived on (system bus)
   * @param message      The incoming QDBusMessage (contains the caller's service name)
   * @param actionId     One of the ACTION_* constants above
   * @return true if authorized, false otherwise
   */
  static bool checkAuthorization( const QDBusConnection &connection,
                                  const QDBusMessage &message,
                                  const char *actionId ) noexcept
  {
    try
    {
      const QString sender = message.service();

      // Retrieve the caller's PID
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

      // Try standard polkit unix-process check first
      if ( checkPolkitSubjectProcess( connection, callerPid, actionId ) )
        return true;

      // Polkit denied; retrieve the caller's UID for fallback checks.
      QDBusMessage getUid = QDBusMessage::createMethodCall(
          "org.freedesktop.DBus",
          "/org/freedesktop/DBus",
          "org.freedesktop.DBus",
          "GetConnectionUnixUser" );
      getUid << sender;

      QDBusReply< uint > uidReply = connection.call( getUid );
      if ( not uidReply.isValid() )
      {
        syslog( LOG_NOTICE, "PolkitAuthority: PID %u denied for action '%s' "
                "(could not resolve UID)",
                callerPid, actionId );
        return false;
      }
      const uint callerUid = uidReply.value();

      // Fallback: try polkit with the user's active session as subject.
      const QString activeSession = findActiveSessionForUid( connection, callerUid );
      if ( !activeSession.isEmpty() &&
           checkPolkitSubjectSession( connection, activeSession, actionId ) )
      {
        syslog( LOG_NOTICE, "PolkitAuthority: PID %u authorized via active session '%s'",
                callerPid, activeSession.toStdString().c_str() );
        return true;
      }

      // Last resort for non-dangerous actions (ACTION_READ / ACTION_CONTROL):
      // Polkit's allow_active=yes means "allow local users without a password".
      // If the process is in a non-active logind session (e.g. IDE terminal)
      // polkit wrongly denies it even though the user IS at the machine.
      // Treat "UID has any session on a physical seat" as equivalent.
      if ( ( std::strcmp( actionId, ACTION_READ ) == 0
             || std::strcmp( actionId, ACTION_CONTROL ) == 0 )
           && hasSessionOnSeat( connection, callerUid ) )
      {
        syslog( LOG_NOTICE, "PolkitAuthority: PID %u (uid %u) authorized for '%s' "
                "(local seat user fallback)",
                callerPid, callerUid, actionId );
        return true;
      }

      syslog( LOG_NOTICE, "PolkitAuthority: PID %u (uid %u) denied for action '%s'",
              callerPid, callerUid, actionId );
      return false;
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

private:
  /// Call polkit CheckAuthorization with a unix-process subject.
  static bool checkPolkitSubjectProcess( const QDBusConnection &connection,
                                         uint pid,
                                         const char *actionId ) noexcept
  {
    QVariantMap subjectDetails;
    subjectDetails["pid"] = QVariant::fromValue( pid );
    subjectDetails["start-time"] = QVariant::fromValue( static_cast< quint64 >( 0 ) );

    QDBusArgument subject;
    subject.beginStructure();
    subject << QString( "unix-process" ) << subjectDetails;
    subject.endStructure();

    return invokePolkitCheck( connection, subject, actionId );
  }

  /// Call polkit CheckAuthorization with a unix-session subject.
  static bool checkPolkitSubjectSession( const QDBusConnection &connection,
                                         const QString &sessionId,
                                         const char *actionId ) noexcept
  {
    QVariantMap subjectDetails;
    subjectDetails["session-id"] = sessionId;

    QDBusArgument subject;
    subject.beginStructure();
    subject << QString( "unix-session" ) << subjectDetails;
    subject.endStructure();

    return invokePolkitCheck( connection, subject, actionId );
  }

  /// Invoke polkit CheckAuthorization with an already-built subject.
  static bool invokePolkitCheck( const QDBusConnection &connection,
                                 const QDBusArgument &subject,
                                 const char *actionId ) noexcept
  {
    QDBusMessage polkitCall = QDBusMessage::createMethodCall(
        "org.freedesktop.PolicyKit1",
        "/org/freedesktop/PolicyKit1/Authority",
        "org.freedesktop.PolicyKit1.Authority",
        "CheckAuthorization" );

    QDBusArgument details;
    details.beginMap( QMetaType::fromType< QString >(), QMetaType::fromType< QString >() );
    details.endMap();

    quint32 flags = 0x1; // AllowUserInteraction
    QString cancellationId;

    polkitCall << QVariant::fromValue( subject )
               << QString( actionId )
               << QVariant::fromValue( details )
               << flags
               << cancellationId;

    QDBusMessage polkitReply = connection.call( polkitCall, QDBus::Block, 60000 );

    if ( polkitReply.type() == QDBusMessage::ErrorMessage )
      return false;

    const QVariant argVariant = polkitReply.arguments().value( 0 );
    const QDBusArgument resultArg = argVariant.value< QDBusArgument >();

    bool isAuthorized = false;
    bool isChallenge = false;
    QMap< QString, QString > resultDetails;

    resultArg.beginStructure();
    resultArg >> isAuthorized >> isChallenge >> resultDetails;
    resultArg.endStructure();

    return isAuthorized;
  }

  /// Query logind over D-Bus to find an active session owned by the given UID.
  static QString findActiveSessionForUid( const QDBusConnection &connection,
                                          uint uid ) noexcept
  {
    const auto sessions = listSessionsForUid( connection, uid );
    for ( const auto &[sessionId, seatId, objectPath] : sessions )
    {
      if ( seatId.isEmpty() )
        continue;

      QDBusMessage propMsg = QDBusMessage::createMethodCall(
          "org.freedesktop.login1",
          objectPath,
          "org.freedesktop.DBus.Properties",
          "Get" );
      propMsg << QStringLiteral( "org.freedesktop.login1.Session" )
              << QStringLiteral( "Active" );

      QDBusReply< QVariant > propReply = connection.call( propMsg, QDBus::Block, 2000 );
      if ( propReply.isValid() && propReply.value().toBool() )
        return sessionId;
    }
    return {};
  }

  /// Check whether the given UID owns any logind session on a physical seat.
  static bool hasSessionOnSeat( const QDBusConnection &connection,
                                uint uid ) noexcept
  {
    const auto sessions = listSessionsForUid( connection, uid );
    for ( const auto &[sessionId, seatId, objectPath] : sessions )
    {
      Q_UNUSED( sessionId )
      Q_UNUSED( objectPath )
      if ( !seatId.isEmpty() )
        return true;
    }
    return false;
  }

  struct SessionInfo
  {
    QString sessionId;
    QString seatId;
    QString objectPath;
  };

  /// List all logind sessions for a given UID.
  static std::vector< SessionInfo > listSessionsForUid( const QDBusConnection &connection,
                                                         uint uid ) noexcept
  {
    std::vector< SessionInfo > result;

    QDBusMessage listMsg = QDBusMessage::createMethodCall(
        "org.freedesktop.login1",
        "/org/freedesktop/login1",
        "org.freedesktop.login1.Manager",
        "ListSessions" );

    QDBusMessage reply = connection.call( listMsg, QDBus::Block, 5000 );
    if ( reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty() )
      return result;

    const QDBusArgument arg = reply.arguments().at( 0 ).value< QDBusArgument >();
    arg.beginArray();
    while ( !arg.atEnd() )
    {
      arg.beginStructure();
      QString sessionId;
      uint sessionUid = 0;
      QString userName, seatId;
      QDBusObjectPath objectPath;
      arg >> sessionId >> sessionUid >> userName >> seatId >> objectPath;
      arg.endStructure();

      if ( sessionUid == uid )
        result.push_back( { sessionId, seatId, objectPath.path() } );
    }
    arg.endArray();

    return result;
  }
};
