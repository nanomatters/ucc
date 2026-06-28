# SPDX-License-Identifier: GPL-3.0-or-later
# Unit test suite — uses Qt Test (Qt6::Test)

set( CMAKE_AUTOMOC ON )

# uccd headers needed by most tests
set( UCCD_INC_DIR "${CMAKE_SOURCE_DIR}/uccd/inc" )

# nlohmann/json lives in uccd/3rdparty
set( UCCD_3RDPARTY_DIR "${CMAKE_SOURCE_DIR}/uccd/3rdparty" )

# ---------- helper function -------------------------------------------------
# ucc_add_test( <name> <source> [LINK_LIBS lib1 lib2 ...] )
#
# Creates an executable, links Qt6::Test + any extra libs, registers it with
# CTest, and adds the uccd include paths automatically.
function( ucc_add_test TEST_NAME TEST_SOURCE )
  cmake_parse_arguments( ARG "" "" "LINK_LIBS" ${ARGN} )

  add_executable( ${TEST_NAME} ${TEST_SOURCE} )

  target_include_directories( ${TEST_NAME} PRIVATE
    ${UCCD_INC_DIR}
    ${UCCD_3RDPARTY_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}/include
  )

  target_link_libraries( ${TEST_NAME} PRIVATE
    Qt6::Core
    Qt6::Test
    ${ARG_LINK_LIBS}
  )

  add_test( NAME ${TEST_NAME} COMMAND ${TEST_NAME} )
endfunction()

# ---------- tests -----------------------------------------------------------

ucc_add_test( test_state_utils      test_state_utils.cpp )
ucc_add_test( test_fan_profile      test_fan_profile.cpp )
ucc_add_test( test_profile_manager  test_profile_manager.cpp )
ucc_add_test( test_settings_manager test_settings_manager.cpp )
ucc_add_test( test_metrics_history  test_metrics_history.cpp )
ucc_add_test( test_uniwill_sysfs    test_uniwill_sysfs.cpp )
