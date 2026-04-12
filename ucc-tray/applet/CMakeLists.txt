# SPDX-License-Identifier: GPL-3.0-or-later

if("${CMAKE_LIBRARY_OUTPUT_DIRECTORY}" STREQUAL "")
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
endif()

plasma_add_applet( com.uniwill.ucc
  QML_SOURCES
    main.qml
    DashboardTab.qml
    PowerProfileTab.qml
    HardwareTab.qml
  CPP_SOURCES
    ../src/TrayBackend.cpp
    ../src/TrayBackend.hpp
  GENERATE_APPLET_CLASS
)

target_include_directories( com.uniwill.ucc PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
  ${CMAKE_CURRENT_SOURCE_DIR}/../../libucc-dbus
)

target_link_libraries( com.uniwill.ucc PRIVATE
  Qt6::DBus
)

if(TARGET ucc-dbus)
  add_dependencies( com.uniwill.ucc ucc-dbus )
  target_link_libraries( com.uniwill.ucc PRIVATE $<TARGET_FILE:ucc-dbus> )
endif()
