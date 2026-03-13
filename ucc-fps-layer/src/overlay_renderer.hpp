// SPDX-License-Identifier: GPL-3.0-or-later
//
// overlay_renderer.hpp — CPU-rendered bitmap-font overlay blitted onto
// swapchain images via vkCmdCopyBufferToImage.  Reads UccOverlayData from
// POSIX shared memory written by uccd during Auto-OC / Auto-Undervolt scans.

#pragma once

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <UccOverlayData.hpp>
#include "font8x8_basic.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace ucc_overlay {

// ─── Layout constants ───────────────────────────────────────────────────────

static constexpr int kScale    = 2;
static constexpr int kCharW    = 8 * kScale;   // 16
static constexpr int kCharH    = 8 * kScale;   // 16
static constexpr int kMaxCols  = 44;
static constexpr int kMaxRows  = 8;
static constexpr int kPadX     = 6;
static constexpr int kPadY     = 4;
static constexpr int kOverlayW = kMaxCols * kCharW + 2 * kPadX;  // 716
static constexpr int kOverlayH = kMaxRows * kCharH + 2 * kPadY;  // 136
static constexpr int kMarginX  = 12;
static constexpr int kMarginY  = 12;
static constexpr uint32_t kStagingBytes = kOverlayW * kOverlayH * 4;

// ─── Colour helpers ─────────────────────────────────────────────────────────

static inline bool is_bgra( VkFormat f )
{
  return f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB ||
         f == VK_FORMAT_B8G8R8A8_SNORM;
}

// Pack RGBA into the uint32 layout matching the swapchain image format.
//   B8G8R8A8 little-endian: bytes B,G,R,A → uint32 = (A<<24)|(R<<16)|(G<<8)|B
//   R8G8B8A8 little-endian: bytes R,G,B,A → uint32 = (A<<24)|(B<<16)|(G<<8)|R
static inline uint32_t pack( uint8_t r, uint8_t g, uint8_t b, uint8_t a, bool bgra )
{
  if ( bgra )
    return ( uint32_t( a ) << 24 ) | ( uint32_t( r ) << 16 ) |
           ( uint32_t( g ) << 8 )  |   uint32_t( b );
  else
    return ( uint32_t( a ) << 24 ) | ( uint32_t( b ) << 16 ) |
           ( uint32_t( g ) << 8 )  |   uint32_t( r );
}

// ─── Shared-memory reader ───────────────────────────────────────────────────

static const UccOverlayData *g_shm_ptr  = nullptr;
static int                   g_shm_fd   = -1;
static int                   g_shm_miss = 0;

static inline const UccOverlayData *read_shm()
{
  if ( g_shm_ptr )
    return g_shm_ptr->active ? g_shm_ptr : nullptr;

  if ( ++g_shm_miss < 120 )   // retry every ~2 s at 60 fps
    return nullptr;
  g_shm_miss = 0;

  g_shm_fd = ::shm_open( kUccOverlayShmName, O_RDONLY, 0 );
  if ( g_shm_fd < 0 )
    return nullptr;

  void *p = ::mmap( nullptr, sizeof( UccOverlayData ),
                    PROT_READ, MAP_SHARED, g_shm_fd, 0 );
  if ( p == MAP_FAILED )
  {
    ::close( g_shm_fd );
    g_shm_fd = -1;
    return nullptr;
  }
  g_shm_ptr = static_cast<const UccOverlayData *>( p );
  return g_shm_ptr->active ? g_shm_ptr : nullptr;
}

// ─── CPU text rendering ─────────────────────────────────────────────────────

static inline void draw_char( uint32_t *buf, int stride, int px, int py,
                              char c, uint32_t color, int maxW, int maxH )
{
  if ( c < 32 || c > 126 ) c = '?';
  const unsigned char *glyph = font8x8_basic[ static_cast<int>( c ) ];
  for ( int row = 0; row < 8; ++row )
  {
    unsigned char bits = glyph[ row ];
    for ( int col = 0; col < 8; ++col )
    {
      if ( !( bits & ( 1 << col ) ) )
        continue;
      for ( int sy = 0; sy < kScale; ++sy )
        for ( int sx = 0; sx < kScale; ++sx )
        {
          int x = px + col * kScale + sx;
          int y = py + row * kScale + sy;
          if ( x >= 0 && x < maxW && y >= 0 && y < maxH )
            buf[ y * stride + x ] = color;
        }
    }
  }
}

static inline void draw_str( uint32_t *buf, int stride, int col, int row,
                             const char *text, uint32_t color,
                             int maxW, int maxH )
{
  int px = kPadX + col * kCharW;
  int py = kPadY + row * kCharH;
  for ( int i = 0; text[ i ] && ( col + i ) < kMaxCols; ++i )
    draw_char( buf, stride, px + i * kCharW, py, text[ i ], color, maxW, maxH );
}

static inline void render_overlay( uint32_t *buf,
                                   const UccOverlayData &d,
                                   VkFormat fmt )
{
  bool bgra  = is_bgra( fmt );
  uint32_t cBg    = pack(   0,   0,   0, 0xCC, bgra );
  uint32_t cText  = pack( 255, 255, 255, 0xFF, bgra );
  uint32_t cLabel = pack( 170, 170, 170, 0xFF, bgra );
  uint32_t cGreen = pack(   0, 220,   0, 0xFF, bgra );
  uint32_t cRed   = pack( 255,  60,  60, 0xFF, bgra );
  uint32_t cTitle = pack( 100, 180, 255, 0xFF, bgra );

  for ( int i = 0; i < kOverlayW * kOverlayH; ++i )
    buf[ i ] = cBg;

  char line[ 64 ];
  int r = 0;

  // ── title ──
  if ( d.mode == 0 )
  {
    const char *ph = "Idle";
    switch ( d.phase )
    {
    case 1: ph = "Baseline";   break;
    case 2: ph = "Searching";  break;
    case 4: ph = "Validating"; break;
    case 5: ph = "Done";       break;
    }
    std::snprintf( line, sizeof( line ), "UCC Auto-OC: %s", ph );
  }
  else
  {
    const char *ph = "Idle";
    switch ( d.phase )
    {
    case 1: ph = "Baseline";      break;
    case 2: ph = "Searching";     break;
    case 3: ph = "Offset Search"; break;
    case 4: ph = "Validating";    break;
    case 5: ph = "Done";          break;
    }
    std::snprintf( line, sizeof( line ), "UCC Undervolt: %s", ph );
  }
  draw_str( buf, kOverlayW, 0, r++, line, cTitle, kOverlayW, kOverlayH );

  // ── progress ──
  if ( d.maxIterations > 0 )
    std::snprintf( line, sizeof( line ), "Step %d / %d",
                   d.iteration, d.maxIterations );
  else
    std::snprintf( line, sizeof( line ), "Step %d", d.iteration );
  draw_str( buf, kOverlayW, 0, r++, line, cLabel, kOverlayW, kOverlayH );

  // ── offset / cap ──
  if ( d.mode == 0 )
    std::snprintf( line, sizeof( line ), "Offset: %+d MHz  Best: %+d MHz",
                   d.currentOffsetMHz, d.bestStableMHz );
  else
    std::snprintf( line, sizeof( line ), "Cap: %d MHz  Best: %d MHz",
                   d.currentOffsetMHz, d.bestStableMHz );
  draw_str( buf, kOverlayW, 0, r++, line, cText, kOverlayW, kOverlayH );

  // ── telemetry ──
  std::snprintf( line, sizeof( line ), "Clock: %d MHz  Temp: %d C",
                 d.gpuClockMHz, d.tempC );
  draw_str( buf, kOverlayW, 0, r++, line, cLabel, kOverlayW, kOverlayH );

  std::snprintf( line, sizeof( line ), "GPU: %d%%  Power: %dW  FPS: %.1f",
                 d.gpuUtilPct, d.powerDrawW, d.fps );
  draw_str( buf, kOverlayW, 0, r++, line, cLabel, kOverlayW, kOverlayH );

  // ── last result ──
  const char *res = "Stable";
  uint32_t rc = cGreen;
  switch ( d.lastResult )
  {
  case 1: res = "Unstable";      rc = cRed; break;
  case 2: res = "Thermal Limit"; rc = cRed; break;
  case 3: res = "Aborted";       rc = cRed; break;
  }
  std::snprintf( line, sizeof( line ), "Last: %s", res );
  draw_str( buf, kOverlayW, 0, r++, line, rc, kOverlayW, kOverlayH );

  // ── message ──
  if ( d.message[ 0 ] )
  {
    char msg[ 128 ];
    std::snprintf( msg, sizeof( msg ), "%s", d.message );
    if ( std::strlen( msg ) > static_cast<size_t>( kMaxCols ) )
      msg[ kMaxCols ] = '\0';
    draw_str( buf, kOverlayW, 0, r++, msg, cLabel, kOverlayW, kOverlayH );
  }
}

// ─── Per-device Vulkan resources ────────────────────────────────────────────

struct DeviceOverlay
{
  VkDevice                             device = VK_NULL_HANDLE;
  PFN_vkGetDeviceProcAddr              gdpa   = nullptr;
  VkPhysicalDeviceMemoryProperties     memProps{};

  bool           ready      = false;
  VkCommandPool  cmdPool    = VK_NULL_HANDLE;
  VkCommandBuffer cmdBuf    = VK_NULL_HANDLE;
  VkFence        fence      = VK_NULL_HANDLE;
  VkBuffer       stagingBuf = VK_NULL_HANDLE;
  VkDeviceMemory stagingMem = VK_NULL_HANDLE;
  void          *mapped     = nullptr;

  // Resolved device functions
  PFN_vkCreateCommandPool          fnCreateCmdPool{};
  PFN_vkAllocateCommandBuffers     fnAllocCmdBufs{};
  PFN_vkBeginCommandBuffer         fnBeginCmdBuf{};
  PFN_vkEndCommandBuffer           fnEndCmdBuf{};
  PFN_vkResetCommandBuffer         fnResetCmdBuf{};
  PFN_vkCmdPipelineBarrier         fnCmdBarrier{};
  PFN_vkCmdCopyBufferToImage       fnCmdCopyBufToImg{};
  PFN_vkQueueSubmit                fnQueueSubmit{};
  PFN_vkCreateFence                fnCreateFence{};
  PFN_vkWaitForFences              fnWaitFences{};
  PFN_vkResetFences                fnResetFences{};
  PFN_vkCreateBuffer               fnCreateBuf{};
  PFN_vkGetBufferMemoryRequirements fnGetBufMemReqs{};
  PFN_vkAllocateMemory             fnAllocMem{};
  PFN_vkBindBufferMemory           fnBindBufMem{};
  PFN_vkMapMemory                  fnMapMem{};
  PFN_vkDestroyBuffer              fnDestroyBuf{};
  PFN_vkFreeMemory                 fnFreeMem{};
  PFN_vkFreeCommandBuffers         fnFreeCmdBufs{};
  PFN_vkDestroyCommandPool         fnDestroyCmdPool{};
  PFN_vkDestroyFence               fnDestroyFence{};
  PFN_vkDeviceWaitIdle             fnDeviceWaitIdle{};

  // Swapchain tracking
  struct SwapInfo
  {
    VkFormat             format;
    uint32_t             width, height;
    std::vector<VkImage> images;
  };
  struct SwapHash
  {
    std::size_t operator()( VkSwapchainKHR s ) const
    { return std::hash<std::uintptr_t>{}( reinterpret_cast<std::uintptr_t>( s ) ); }
  };
  std::unordered_map<VkSwapchainKHR, SwapInfo, SwapHash> swapchains;
};

static std::unordered_map<void *, DeviceOverlay> g_overlays;
static std::mutex                                g_overlay_mtx;

// ─── Vulkan resource helpers ────────────────────────────────────────────────

static inline uint32_t
find_host_visible( const VkPhysicalDeviceMemoryProperties &props,
                   uint32_t typeBits )
{
  constexpr auto flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  for ( uint32_t i = 0; i < props.memoryTypeCount; ++i )
    if ( ( typeBits & ( 1u << i ) ) &&
         ( props.memoryTypes[ i ].propertyFlags & flags ) == flags )
      return i;
  return UINT32_MAX;
}

#define UCC_RESOLVE( dst, name )                                          \
  do {                                                                    \
    dst = reinterpret_cast<decltype( dst )>(                              \
      ov.gdpa( ov.device, #name ) );                                     \
    if ( !dst ) return false;                                             \
  } while ( 0 )

static bool init_resources( DeviceOverlay &ov, uint32_t queueFamily )
{
  UCC_RESOLVE( ov.fnCreateCmdPool,   vkCreateCommandPool );
  UCC_RESOLVE( ov.fnAllocCmdBufs,    vkAllocateCommandBuffers );
  UCC_RESOLVE( ov.fnBeginCmdBuf,     vkBeginCommandBuffer );
  UCC_RESOLVE( ov.fnEndCmdBuf,       vkEndCommandBuffer );
  UCC_RESOLVE( ov.fnResetCmdBuf,     vkResetCommandBuffer );
  UCC_RESOLVE( ov.fnCmdBarrier,      vkCmdPipelineBarrier );
  UCC_RESOLVE( ov.fnCmdCopyBufToImg, vkCmdCopyBufferToImage );
  UCC_RESOLVE( ov.fnQueueSubmit,     vkQueueSubmit );
  UCC_RESOLVE( ov.fnCreateFence,     vkCreateFence );
  UCC_RESOLVE( ov.fnWaitFences,      vkWaitForFences );
  UCC_RESOLVE( ov.fnResetFences,     vkResetFences );
  UCC_RESOLVE( ov.fnCreateBuf,       vkCreateBuffer );
  UCC_RESOLVE( ov.fnGetBufMemReqs,   vkGetBufferMemoryRequirements );
  UCC_RESOLVE( ov.fnAllocMem,        vkAllocateMemory );
  UCC_RESOLVE( ov.fnBindBufMem,      vkBindBufferMemory );
  UCC_RESOLVE( ov.fnMapMem,          vkMapMemory );
  UCC_RESOLVE( ov.fnDestroyBuf,      vkDestroyBuffer );
  UCC_RESOLVE( ov.fnFreeMem,         vkFreeMemory );
  UCC_RESOLVE( ov.fnFreeCmdBufs,     vkFreeCommandBuffers );
  UCC_RESOLVE( ov.fnDestroyCmdPool,  vkDestroyCommandPool );
  UCC_RESOLVE( ov.fnDestroyFence,    vkDestroyFence );
  UCC_RESOLVE( ov.fnDeviceWaitIdle,  vkDeviceWaitIdle );

  // Command pool
  VkCommandPoolCreateInfo poolCI{};
  poolCI.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  poolCI.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  poolCI.queueFamilyIndex = queueFamily;
  if ( ov.fnCreateCmdPool( ov.device, &poolCI, nullptr, &ov.cmdPool ) != VK_SUCCESS )
    return false;

  // Command buffer
  VkCommandBufferAllocateInfo cbAI{};
  cbAI.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cbAI.commandPool        = ov.cmdPool;
  cbAI.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbAI.commandBufferCount = 1;
  if ( ov.fnAllocCmdBufs( ov.device, &cbAI, &ov.cmdBuf ) != VK_SUCCESS )
    return false;

  // Fence — signaled so the first WaitForFences returns immediately
  VkFenceCreateInfo fCI{};
  fCI.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  fCI.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  if ( ov.fnCreateFence( ov.device, &fCI, nullptr, &ov.fence ) != VK_SUCCESS )
    return false;

  // Staging buffer
  VkBufferCreateInfo bufCI{};
  bufCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bufCI.size  = kStagingBytes;
  bufCI.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  if ( ov.fnCreateBuf( ov.device, &bufCI, nullptr, &ov.stagingBuf ) != VK_SUCCESS )
    return false;

  VkMemoryRequirements memReqs;
  ov.fnGetBufMemReqs( ov.device, ov.stagingBuf, &memReqs );

  uint32_t memIdx = find_host_visible( ov.memProps, memReqs.memoryTypeBits );
  if ( memIdx == UINT32_MAX )
    return false;

  VkMemoryAllocateInfo mai{};
  mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  mai.allocationSize  = memReqs.size;
  mai.memoryTypeIndex = memIdx;
  if ( ov.fnAllocMem( ov.device, &mai, nullptr, &ov.stagingMem ) != VK_SUCCESS )
    return false;

  if ( ov.fnBindBufMem( ov.device, ov.stagingBuf, ov.stagingMem, 0 ) != VK_SUCCESS )
    return false;
  if ( ov.fnMapMem( ov.device, ov.stagingMem, 0, kStagingBytes, 0, &ov.mapped ) != VK_SUCCESS )
    return false;

  ov.ready = true;
  return true;
}

#undef UCC_RESOLVE

static inline void destroy_resources( DeviceOverlay &ov )
{
  if ( !ov.ready )
    return;
  if ( ov.fnDeviceWaitIdle )
    ov.fnDeviceWaitIdle( ov.device );
  if ( ov.stagingBuf && ov.fnDestroyBuf )
    ov.fnDestroyBuf( ov.device, ov.stagingBuf, nullptr );
  if ( ov.stagingMem && ov.fnFreeMem )
    ov.fnFreeMem( ov.device, ov.stagingMem, nullptr );
  if ( ov.cmdBuf && ov.fnFreeCmdBufs )
    ov.fnFreeCmdBufs( ov.device, ov.cmdPool, 1, &ov.cmdBuf );
  if ( ov.cmdPool && ov.fnDestroyCmdPool )
    ov.fnDestroyCmdPool( ov.device, ov.cmdPool, nullptr );
  if ( ov.fence && ov.fnDestroyFence )
    ov.fnDestroyFence( ov.device, ov.fence, nullptr );
  ov.ready = false;
  ov.mapped = nullptr;
}

// ─── Public API — called from layer.cpp ─────────────────────────────────────

/// Register a newly-created device for overlay support.
inline void init_device( void *deviceKey, VkDevice device,
                         PFN_vkGetDeviceProcAddr gdpa,
                         const VkPhysicalDeviceMemoryProperties &memProps )
{
  std::lock_guard lk( g_overlay_mtx );
  auto &ov    = g_overlays[ deviceKey ];
  ov.device   = device;
  ov.gdpa     = gdpa;
  ov.memProps = memProps;
}

/// Tear down overlay resources before the device is destroyed.
inline void destroy_device( void *deviceKey )
{
  std::lock_guard lk( g_overlay_mtx );
  auto it = g_overlays.find( deviceKey );
  if ( it == g_overlays.end() )
    return;
  destroy_resources( it->second );
  g_overlays.erase( it );
}

/// Record swapchain format/extent for overlay placement.
inline void register_swapchain( void *deviceKey, VkSwapchainKHR sc,
                                VkFormat fmt, uint32_t w, uint32_t h )
{
  std::lock_guard lk( g_overlay_mtx );
  auto it = g_overlays.find( deviceKey );
  if ( it == g_overlays.end() )
    return;
  it->second.swapchains[ sc ] = { fmt, w, h, {} };
}

/// Store the swapchain image handles for overlay blitting.
inline void register_swapchain_images( void *deviceKey, VkSwapchainKHR sc,
                                       uint32_t count, const VkImage *images )
{
  std::lock_guard lk( g_overlay_mtx );
  auto dit = g_overlays.find( deviceKey );
  if ( dit == g_overlays.end() )
    return;
  auto sit = dit->second.swapchains.find( sc );
  if ( sit == dit->second.swapchains.end() )
    return;
  sit->second.images.assign( images, images + count );
}

/// Forget a swapchain (called before destruction).
inline void unregister_swapchain( void *deviceKey, VkSwapchainKHR sc )
{
  std::lock_guard lk( g_overlay_mtx );
  auto it = g_overlays.find( deviceKey );
  if ( it == g_overlays.end() )
    return;
  it->second.swapchains.erase( sc );
}

/// Render the overlay onto swapchain images just before the real present.
/// Acquires g_overlay_mtx internally — do NOT hold the layer dispatch lock.
inline void before_present( VkQueue queue, void *deviceKey,
                            uint32_t queueFamily,
                            uint32_t swapchainCount,
                            const VkSwapchainKHR *swapchains,
                            const uint32_t *imageIndices )
{
  const UccOverlayData *data = read_shm();
  if ( !data )
    return;

  std::lock_guard lk( g_overlay_mtx );
  auto dit = g_overlays.find( deviceKey );
  if ( dit == g_overlays.end() )
    return;
  auto &ov = dit->second;

  // Lazy-init on first overlay frame
  if ( !ov.ready )
  {
    if ( !init_resources( ov, queueFamily ) )
      return;
  }

  // Wait for previous overlay command to finish (first call returns immediately
  // because the fence was created signaled).
  ov.fnWaitFences( ov.device, 1, &ov.fence, VK_TRUE, UINT64_MAX );
  ov.fnResetFences( ov.device, 1, &ov.fence );

  // Pick swapchain format for text rendering
  VkFormat renderFmt = VK_FORMAT_B8G8R8A8_UNORM;
  for ( uint32_t i = 0; i < swapchainCount; ++i )
  {
    auto sit = ov.swapchains.find( swapchains[ i ] );
    if ( sit != ov.swapchains.end() )
    {
      renderFmt = sit->second.format;
      break;
    }
  }

  // CPU text render → staging buffer
  render_overlay( static_cast<uint32_t *>( ov.mapped ), *data, renderFmt );

  // Record one command buffer for all swapchain images
  ov.fnResetCmdBuf( ov.cmdBuf, 0 );

  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  ov.fnBeginCmdBuf( ov.cmdBuf, &beginInfo );

  bool anyRecorded = false;

  for ( uint32_t i = 0; i < swapchainCount; ++i )
  {
    auto sit = ov.swapchains.find( swapchains[ i ] );
    if ( sit == ov.swapchains.end() )
      continue;
    const auto &sw = sit->second;
    if ( imageIndices[ i ] >= static_cast<uint32_t>( sw.images.size() ) )
      continue;

    uint32_t ox = static_cast<uint32_t>( kMarginX );
    uint32_t oy = static_cast<uint32_t>( kMarginY );
    uint32_t ow = static_cast<uint32_t>( kOverlayW );
    uint32_t oh = static_cast<uint32_t>( kOverlayH );
    if ( ox + ow > sw.width || oy + oh > sw.height )
      continue;

    VkImage image = sw.images[ imageIndices[ i ] ];

    // Barrier: PRESENT_SRC_KHR → TRANSFER_DST_OPTIMAL
    VkImageMemoryBarrier bar1{};
    bar1.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar1.srcAccessMask       = VK_ACCESS_MEMORY_READ_BIT;
    bar1.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    bar1.oldLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    bar1.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar1.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar1.image               = image;
    bar1.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    ov.fnCmdBarrier( ov.cmdBuf,
                     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                     0, 0, nullptr, 0, nullptr, 1, &bar1 );

    // Copy staging buffer → swapchain image
    VkBufferImageCopy region{};
    region.bufferOffset      = 0;
    region.bufferRowLength   = kOverlayW;
    region.bufferImageHeight = kOverlayH;
    region.imageSubresource  = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageOffset       = { static_cast<int32_t>( ox ),
                                 static_cast<int32_t>( oy ), 0 };
    region.imageExtent       = { ow, oh, 1 };

    ov.fnCmdCopyBufToImg( ov.cmdBuf, ov.stagingBuf, image,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          1, &region );

    // Barrier: TRANSFER_DST_OPTIMAL → PRESENT_SRC_KHR
    VkImageMemoryBarrier bar2{};
    bar2.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar2.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    bar2.dstAccessMask       = VK_ACCESS_MEMORY_READ_BIT;
    bar2.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar2.newLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    bar2.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar2.image               = image;
    bar2.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    ov.fnCmdBarrier( ov.cmdBuf,
                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                     0, 0, nullptr, 0, nullptr, 1, &bar2 );

    anyRecorded = true;
  }

  ov.fnEndCmdBuf( ov.cmdBuf );

  if ( !anyRecorded )
    return;

  // Submit and wait (stall is acceptable — overlay is active only during scans)
  VkSubmitInfo submit{};
  submit.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers    = &ov.cmdBuf;
  ov.fnQueueSubmit( queue, 1, &submit, ov.fence );
  ov.fnWaitFences( ov.device, 1, &ov.fence, VK_TRUE, UINT64_MAX );
}

} // namespace ucc_overlay
