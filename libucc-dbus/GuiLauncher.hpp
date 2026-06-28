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

#include <QDebug>
#include <QProcess>
#include <QStandardPaths>
#include <QStringList>

namespace ucc
{

/**
 * @brief Launch the UCC GUI (ucc-gui) as a detached process.
 *
 * Resolves the executable via QStandardPaths so the call works regardless of
 * installation prefix. Falls back to invoking via @c /usr/bin/env when the
 * executable is not on the standard search path (e.g. custom installs).
 *
 * @param args Optional arguments forwarded to ucc-gui.
 * @return true if the OS accepted the launch request.
 * @return false if the executable could not be started.
 */
inline bool launchGui( const QStringList &args = {} )
{
  // Note: QProcess does not invoke a shell; arguments must be passed separately.
  // Passing "/usr/bin/env ucc-gui" as a single string would be treated as a
  // literal executable path and fail.
  const QString uccGui = QStandardPaths::findExecutable( "ucc-gui" );

  qint64 pid = 0;
  bool ok = false;
  if ( !uccGui.isEmpty() )
    ok = QProcess::startDetached( uccGui, args, QString{}, &pid );
  else
    ok = QProcess::startDetached( "/usr/bin/env", QStringList{ "ucc-gui" } + args, QString{}, &pid );

  if ( !ok )
    qWarning() << "[ucc::launchGui] Failed to start ucc-gui (found:" << uccGui << ")";

  return ok;
}

} // namespace ucc
