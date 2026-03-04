# SPDX-License-Identifier: GPL-3.0-or-later
#
# ucc-fps-layer – minimal Vulkan implicit layer + GLX hook that streams
# rolling FPS to uccd over a Unix socket during Auto-OC scans.

cmake_minimum_required( VERSION 3.20 )

# ---- Find Vulkan headers (vulkan-headers / libvulkan-dev) ------------------
find_package( Vulkan QUIET )
if( NOT Vulkan_FOUND )
  # Fallback: just check for the header
  find_path( Vulkan_INCLUDE_DIR NAMES vulkan/vulkan.h
    PATHS /usr/include /usr/local/include
    REQUIRED
  )
  message( STATUS "fps-layer: found Vulkan headers at ${Vulkan_INCLUDE_DIR}" )
else()
  message( STATUS "fps-layer: using Vulkan ${Vulkan_VERSION}" )
endif()

# ---- Shared library --------------------------------------------------------
add_library( ucc_fps_layer SHARED
  src/layer.cpp
)

set_target_properties( ucc_fps_layer PROPERTIES
  CXX_STANDARD          20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS        OFF
  # Prevent the linker from adding SONAME; the loader loads by full path.
  NO_SONAME             TRUE
  # Strip symbol visibility – expose only the explicitly extern "C" symbols.
  C_VISIBILITY_PRESET   hidden
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)

target_include_directories( ucc_fps_layer PRIVATE
  ${Vulkan_INCLUDE_DIRS}
)

target_compile_options( ucc_fps_layer PRIVATE
  -fvisibility=hidden
  # Suppress pedantic warnings that fire on the Vulkan headers.
  -Wno-pedantic
  -Wno-old-style-cast
)

target_link_libraries( ucc_fps_layer PRIVATE dl pthread )

# ---- Layer manifest --------------------------------------------------------
# CMAKE_INSTALL_LIBDIR may already be an absolute path (e.g. /usr/lib64 on
# Fedora/RHEL) or a relative one (e.g. lib on Debian).  Compute the final
# absolute path once so the JSON template never has to combine the two.
if( IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}" )
  set( UCC_FPS_LAYER_LIBDIR "${CMAKE_INSTALL_LIBDIR}" )
else()
  set( UCC_FPS_LAYER_LIBDIR "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}" )
endif()

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/VkLayer_ucc_fps.json.in"
  "${CMAKE_CURRENT_BINARY_DIR}/VkLayer_ucc_fps.json"
  @ONLY
)

# ---- Install ---------------------------------------------------------------
include( GNUInstallDirs )

install( TARGETS ucc_fps_layer
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)

install( FILES "${CMAKE_CURRENT_BINARY_DIR}/VkLayer_ucc_fps.json"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/vulkan/implicit_layer.d"
)

message( STATUS "fps-layer: will install layer to ${UCC_FPS_LAYER_LIBDIR}/libucc_fps_layer.so" )
