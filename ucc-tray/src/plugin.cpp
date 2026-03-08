/*
 * SPDX-FileCopyrightText: 2026 Unified Control Center Contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "plugin.h"
#include "TrayBackend.hpp"

#include <QtQml>

void Plugin::registerTypes( const char *uri )
{
  qmlRegisterType< TrayBackend >( uri, 0, 1, "TrayBackend" );
}

#include "moc_plugin.cpp"
