# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required( VERSION 3.20 )

project( ucc
  VERSION 0.1.0
  DESCRIPTION "Uniwill Control Center - Qt/KDE application suite"
  LANGUAGES CXX
)

# C++ Standard
set( CMAKE_CXX_STANDARD 20 )
set( CMAKE_CXX_STANDARD_REQUIRED ON )
set( CMAKE_CXX_EXTENSIONS OFF )

# Build type
if( NOT CMAKE_BUILD_TYPE )
  set( CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE )
endif()

# Compiler flags
add_compile_options(
  -Wall
  -Wextra
  -Wpedantic
)

if(CMAKE_BUILD_TYPE STREQUAL "DEBUG")
  add_compile_options(-O0 -g3)
elseif(CMAKE_BUILD_TYPE STREQUAL "RELEASE")
  add_compile_options(-O3)
elseif(CMAKE_BUILD_TYPE STREQUAL "RELWITHDEBINFO")
  add_compile_options(-O2 -g)
endif()

# Find dependencies
find_package( Qt6 REQUIRED COMPONENTS
  Core
  DBus
  Gui
  Widgets
  Qml
  Quick
  QuickControls2
)

find_package( ECM REQUIRED NO_MODULE )
set( CMAKE_MODULE_PATH ${ECM_MODULE_PATH} )
include( KDEInstallDirs )
include( KDECMakeSettings )

# Options
option( BUILD_GUI "Build the main GUI application" ON )
option( BUILD_TRAY "Build the system tray applet" ON )
option( BUILD_WIDGETS "Build Plasma widgets" OFF )

# Try to find Plasma if widget build is requested
if(BUILD_WIDGETS)
  find_package(KF5Plasma QUIET)
  if(KF5Plasma_FOUND)
    message(STATUS "Plasma found, building widgets.")
    add_subdirectory(ucc-widgets)
  else()
    message(WARNING "Plasma not found, widgets will not be built.")
    set(BUILD_WIDGETS OFF)
  endif()
endif()

# Subdirectories
add_subdirectory( uccd )
add_subdirectory( libucc-dbus )

if( BUILD_GUI )
  add_subdirectory( ucc-gui )
endif()

if( BUILD_TRAY )
  add_subdirectory( ucc-tray )
endif()

# Install SVG icons to /usr/share/pixmaps for legacy compatibility
install(FILES icons/ucc-gui.svg DESTINATION share/pixmaps)
install(FILES icons/ucc-tray.svg DESTINATION share/pixmaps)

# Icon install (all sizes)
install(FILES
  icons/ucc-gui.svg
  icons/ucc-gui_16.svg
  icons/ucc-gui_24.svg
  icons/ucc-gui_32.svg
  icons/ucc-gui_48.svg
  icons/ucc-gui_64.svg
  icons/ucc-gui_128.svg
  icons/ucc-tray.svg
  icons/ucc-tray_16.svg
  icons/ucc-tray_24.svg
  icons/ucc-tray_32.svg
  icons/ucc-tray_48.svg
  icons/ucc-tray_64.svg
  icons/ucc-tray_128.svg
  DESTINATION share/icons/hicolor/scalable/apps
)

# Install PNGs to correct hicolor subdirectories
install(FILES icons/ucc-gui_16.png   DESTINATION share/icons/hicolor/16x16/apps RENAME ucc-gui.png)
install(FILES icons/ucc-gui_24.png   DESTINATION share/icons/hicolor/24x24/apps RENAME ucc-gui.png)
install(FILES icons/ucc-gui_32.png   DESTINATION share/icons/hicolor/32x32/apps RENAME ucc-gui.png)
install(FILES icons/ucc-gui_48.png   DESTINATION share/icons/hicolor/48x48/apps RENAME ucc-gui.png)
install(FILES icons/ucc-gui_64.png   DESTINATION share/icons/hicolor/64x64/apps RENAME ucc-gui.png)
install(FILES icons/ucc-gui_128.png  DESTINATION share/icons/hicolor/128x128/apps RENAME ucc-gui.png)
install(FILES icons/ucc-tray_16.png  DESTINATION share/icons/hicolor/16x16/apps RENAME ucc-tray.png)
install(FILES icons/ucc-tray_24.png  DESTINATION share/icons/hicolor/24x24/apps RENAME ucc-tray.png)
install(FILES icons/ucc-tray_32.png  DESTINATION share/icons/hicolor/32x32/apps RENAME ucc-tray.png)
install(FILES icons/ucc-tray_48.png  DESTINATION share/icons/hicolor/48x48/apps RENAME ucc-tray.png)
install(FILES icons/ucc-tray_64.png  DESTINATION share/icons/hicolor/64x64/apps RENAME ucc-tray.png)
install(FILES icons/ucc-tray_128.png DESTINATION share/icons/hicolor/128x128/apps RENAME ucc-tray.png)

# Summary
message(STATUS "")
message(STATUS "=== Uniwill Control Center Build Configuration ===")
message(STATUS "  Version:          ${PROJECT_VERSION}")
message(STATUS "  Build type:       ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ Standard:     ${CMAKE_CXX_STANDARD}")
message(STATUS "  Qt Version:       ${Qt6_VERSION}")
message(STATUS "")
message(STATUS "  Components:")
message(STATUS "    DBus Library:   ON")
message(STATUS "    GUI App:        ${BUILD_GUI}")
message(STATUS "    Tray App:       ${BUILD_TRAY}")
message(STATUS "    Plasma Widgets: ${BUILD_WIDGETS}")
message(STATUS "")
message(STATUS "  Install prefix:   ${CMAKE_INSTALL_PREFIX}")
message(STATUS "===================================================")
message(STATUS "")
