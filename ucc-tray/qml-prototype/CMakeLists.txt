cmake_minimum_required(VERSION 3.16)
project(ucc-tray-proto LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Quick QuickControls2 Qml)

add_executable(ucc-tray-proto
  main.cpp
)

target_link_libraries(ucc-tray-proto PRIVATE
  Qt6::Quick
  Qt6::QuickControls2
  Qt6::Qml
)

target_compile_definitions(ucc-tray-proto PRIVATE QT_QUICK_CONTROLS_STYLE=\"Material\")

install(TARGETS ucc-tray-proto RUNTIME DESTINATION bin)
