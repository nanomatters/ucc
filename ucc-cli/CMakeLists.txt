# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required( VERSION 3.20 )

project( ucc-cli
  VERSION ${CMAKE_PROJECT_VERSION}
  DESCRIPTION "Unified Control Center command-line interface"
  LANGUAGES CXX
)

set( CMAKE_CXX_STANDARD 20 )
set( CMAKE_CXX_STANDARD_REQUIRED ON )
set( CMAKE_CXX_EXTENSIONS OFF )

set( CMAKE_AUTOMOC ON )

# Find Qt6
find_package( Qt6 REQUIRED COMPONENTS Core DBus )

# Application sources
add_executable( ucc-cli
  main.cpp
)

target_include_directories( ucc-cli PRIVATE
  ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries( ucc-cli PRIVATE
  ucc-dbus
  Qt6::Core
  Qt6::DBus
)

target_compile_options( ucc-cli PRIVATE
  -Wall
  -Wextra
  -Wpedantic
)

# Installation
include( GNUInstallDirs )
install( TARGETS ucc-cli
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Install man page
install( FILES ucc-cli.1
  DESTINATION ${CMAKE_INSTALL_MANDIR}/man1
)

# Install bash completion
install( FILES ucc-cli.bash
  DESTINATION ${CMAKE_INSTALL_DATADIR}/bash-completion/completions
  RENAME ucc-cli
)
