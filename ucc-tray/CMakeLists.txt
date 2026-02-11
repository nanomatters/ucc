# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required( VERSION 3.20 )

project( ucc-tray
  VERSION 0.1.0
  DESCRIPTION "Uniwill Control Center system tray applet"
  LANGUAGES CXX
)

set( CMAKE_CXX_STANDARD 20 )
set( CMAKE_CXX_STANDARD_REQUIRED ON )
set( CMAKE_CXX_EXTENSIONS OFF )

set( CMAKE_AUTOMOC ON )

# Find Qt6
find_package( Qt6 REQUIRED COMPONENTS
  Core
  Gui
  Widgets
  DBus
)

# Application sources
add_executable( ucc-tray
  main.cpp
)

target_include_directories( ucc-tray PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../libucc-dbus
)

# Allow building the tray standalone: if the lib source is present and the
# target doesn't already exist, add the libucc-dbus subproject so the
# ucc-dbus target becomes available here.
if(NOT TARGET ucc-dbus AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../libucc-dbus/CMakeLists.txt)
  message(STATUS "Configuring local libucc-dbus subproject for standalone tray build.")
  add_subdirectory( ${CMAKE_CURRENT_SOURCE_DIR}/../libucc-dbus
                    ${CMAKE_CURRENT_BINARY_DIR}/libucc-dbus )
endif()

target_link_libraries( ucc-tray PRIVATE
  Qt6::Core
  Qt6::Gui
  Qt6::Widgets
  Qt6::DBus
)

# Link libucc-dbus if available as a target, otherwise try to find an installed/shared library
option(UCC_ENABLE_DEV_RPATH "Add build-tree RPATH for standalone run (development only)" OFF)
if(TARGET ucc-dbus)
  # For development only: allow adding a build-tree RPATH so the tray can run directly
  # from the build directory. This MUST remain OFF during packaging to avoid embedding
  # absolute build paths in installed binaries.
  if(UCC_ENABLE_DEV_RPATH)
    target_link_options( ucc-tray PRIVATE "-Wl,-rpath,$<TARGET_FILE_DIR:ucc-dbus>" )
  endif()
  target_link_libraries( ucc-tray PRIVATE $<TARGET_FILE:ucc-dbus> )
else()
  find_library( UCC_DBUS_LIB NAMES ucc-dbus
    PATHS
      ${CMAKE_CURRENT_SOURCE_DIR}/../build/lib
      ${CMAKE_INSTALL_PREFIX}/lib
      /usr/lib
      /usr/lib64
    NO_DEFAULT_PATH
  )
  if(UCC_DBUS_LIB)
    target_link_libraries( ucc-tray PRIVATE ${UCC_DBUS_LIB} )
  else()
    message(WARNING "Could not find libucc-dbus. Build may fail unless libucc-dbus is available or parent project is used to configure.")
  endif()
endif()

target_compile_options( ucc-tray PRIVATE
  -Wall
  -Wextra
  -Wpedantic
  -Werror
)

# Ensure the DBus library is built before the tray so the linker finds it reliably
if(TARGET ucc-dbus)
  add_dependencies( ucc-tray ucc-dbus )
endif()

# Installation
install( TARGETS ucc-tray
  RUNTIME DESTINATION bin
)

# Desktop file for autostart
install( FILES ucc-tray.desktop
  DESTINATION share/applications
)
