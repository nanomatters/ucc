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

# ---- Shared libraries ------------------------------------------------------
add_library( ucc_fps_layer SHARED
  src/layer.cpp
)

add_library( ucc_fps_gl SHARED
  src/opengl_shim.cpp
)

foreach( target IN ITEMS ucc_fps_layer ucc_fps_gl )
  set_target_properties( ${target} PROPERTIES
    CXX_STANDARD          20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS        OFF
    NO_SONAME             TRUE
    C_VISIBILITY_PRESET   hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
  )

  target_compile_options( ${target} PRIVATE
    -fvisibility=hidden
    -Wno-pedantic
    -Wno-old-style-cast
  )

  target_link_libraries( ${target} PRIVATE dl pthread rt )
endforeach()

target_include_directories( ucc_fps_layer PRIVATE
  ${Vulkan_INCLUDE_DIRS}
)

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

# Build-tree runtime manifest for local testing via ucc-fps-hook.
set( _UCC_FPS_LAYER_LIBDIR_SAVE "${UCC_FPS_LAYER_LIBDIR}" )
set( UCC_FPS_LAYER_LIBDIR "${CMAKE_CURRENT_BINARY_DIR}" )
file( MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/runtime" )
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/VkLayer_ucc_fps.json.in"
  "${CMAKE_CURRENT_BINARY_DIR}/runtime/VkLayer_ucc_fps.json"
  @ONLY
)
set( UCC_FPS_LAYER_LIBDIR "${_UCC_FPS_LAYER_LIBDIR_SAVE}" )

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/ucc-fps-hook.in"
  "${CMAKE_CURRENT_BINARY_DIR}/ucc-fps-hook"
  @ONLY
)

file( CHMOD "${CMAKE_CURRENT_BINARY_DIR}/ucc-fps-hook"
  PERMISSIONS
    OWNER_READ OWNER_WRITE OWNER_EXECUTE
    GROUP_READ GROUP_EXECUTE
    WORLD_READ WORLD_EXECUTE
)

# ---- Install ---------------------------------------------------------------
include( GNUInstallDirs )

install( TARGETS ucc_fps_layer ucc_fps_gl
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)

install( FILES "${CMAKE_CURRENT_BINARY_DIR}/VkLayer_ucc_fps.json"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/vulkan/implicit_layer.d"
)

install( PROGRAMS "${CMAKE_CURRENT_BINARY_DIR}/ucc-fps-hook"
  DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

message( STATUS "fps-layer: will install Vulkan layer to ${UCC_FPS_LAYER_LIBDIR}/libucc_fps_layer.so" )
message( STATUS "fps-layer: will install OpenGL shim to ${UCC_FPS_LAYER_LIBDIR}/libucc_fps_gl.so" )
