cmake_minimum_required(VERSION 3.16)
project(uccd)

# Add 3rdparty
add_subdirectory(3rdparty)

# Set default build type to RelWithDebInfo if not specified
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# Set C++20 standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O2")

# Find required packages
find_package(PkgConfig REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Bluetooth DBus)

set(CMAKE_AUTOMOC ON)

# Main executable
add_executable(uccd
  src/uccd.cpp
  src/UccDBusService.cpp
  src/workers/ProfileSettingsWorker.cpp
  src/workers/KeyboardBacklightListener.cpp
  src/workers/LCTWaterCoolerWorker.cpp
  src/workers/DisplayWorker.cpp
  src/workers/HardwareMonitorWorker.cpp
  inc/UccDBusService.hpp
  inc/workers/LCTWaterCoolerWorker.hpp
  inc/workers/DaemonWorker.hpp
  src/profiles/FanProfiles.cpp
  src/profiles/UccProfile.cpp
  src/profiles/DefaultProfiles.cpp
)

# Include directories
target_include_directories(uccd PRIVATE inc/ ${CMAKE_SOURCE_DIR}/../libucc-dbus ${Qt6Core_INCLUDE_DIRS} ${Qt6Bluetooth_INCLUDE_DIRS} ${Qt6DBus_INCLUDE_DIRS} /usr/include)

# Link to tuxedo_io_lib
target_link_libraries(uccd PRIVATE
  tuxedo_io_lib
)

# Link system libraries
target_link_libraries(uccd PRIVATE
  udev
  Qt6::Core
  Qt6::Bluetooth
  Qt6::DBus
)

# Add compile options for stricter checking
target_compile_options(uccd PRIVATE
  -Wall
  -Wextra
  -Wpedantic
  -Wconversion
  -Wshadow
  -Wnon-virtual-dtor
  -Wold-style-cast
  -Wcast-align
  -Wunused
  -Woverloaded-virtual
  -Wdouble-promotion
  -Wformat=2
  -fexceptions
)

# Installation target
install(TARGETS uccd
  RUNTIME DESTINATION bin
)

# Install systemd service files
install(FILES uccd.service uccd-sleep.service
  DESTINATION lib/systemd/system
)

# Install DBus system service file for activation
install(FILES com.uniwill.uccd.service
  DESTINATION share/dbus-1/system-services
)
