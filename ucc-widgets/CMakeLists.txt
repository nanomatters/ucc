# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required( VERSION 3.20 )

project( ucc-widgets
  VERSION 0.1.0
  DESCRIPTION "Plasma widgets for Uniwill Control Center"
  LANGUAGES NONE
)

# Find KDE Plasma
find_package( ECM REQUIRED NO_MODULE )
set( CMAKE_MODULE_PATH ${ECM_MODULE_PATH} )

find_package( Plasma REQUIRED )

include( KDEInstallDirs )
include( KDECMakeSettings )

# Install plasma packages
plasma_install_package( system-monitor org.uniwill.ucc.systemmonitor )
plasma_install_package( profile-switcher org.uniwill.ucc.profileswitcher )

message( STATUS "Plasma widgets will be installed to: ${KDE_INSTALL_DATADIR}/plasma/plasmoids/" )
