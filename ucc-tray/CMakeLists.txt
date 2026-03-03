# SPDX-License-Identifier: GPL-3.0-or-later
#
# ucc-tray — KDE Plasma system-tray applet for Uniwill Control Center
#
# Structure (following plasma-pa pattern):
#   src/     — QML extension module (com.uniwill.ucc.private) providing TrayBackend
#   applet/  — Plasma applet package (com.uniwill.ucc) with the QML UI
#

# This directory hosts a Plasma applet in `applet/` with a QML extension
# module in `src/`.

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.20)

  project(
    ucc-tray
    VERSION 0.2.0
    DESCRIPTION "Uniwill Control Center tray"
    LANGUAGES CXX
  )

  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)

  set(CMAKE_AUTOMOC ON)
  set(CMAKE_AUTORCC ON)

  find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets DBus Qml Quick QuickControls2)

  find_package(ECM REQUIRED NO_MODULE)
  set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
  include(KDEInstallDirs)
  include(KDECMakeSettings)
  include(ECMQmlModule)

  find_package(KF6Config REQUIRED)
  find_package(KF6CoreAddons REQUIRED)
  find_package(Plasma REQUIRED)
endif()

set(_ucc_tray_has_plasma_applet FALSE)
if(
  EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/CMakeLists.txt"
  AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/applet/CMakeLists.txt"
)
  set(_ucc_tray_has_plasma_applet TRUE)
endif()

if(_ucc_tray_has_plasma_applet)
  add_subdirectory(src)
  add_subdirectory(applet)
else()
  message(
    FATAL_ERROR
    "ucc-tray: no buildable sources found (expected src/ + applet/ for Plasma applet)."
  )
endif()
