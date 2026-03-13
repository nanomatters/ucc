// SPDX-License-Identifier: GPL-3.0-or-later
//
// ucc-fps-layer/src/layer.cpp
//
// Minimal Vulkan implicit layer that counts presented frames and streams
// rolling FPS to uccd's FPS socket when explicitly enabled for the target
// process.

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "fps_reporter.hpp"
#include "overlay_renderer.hpp"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <fcntl.h>

static constexpr const char *kLayerName = "VK_LAYER_UCC_FPS";

static bool debug_enabled()
{
  static int enabled = []() {
    const char *v = std::getenv( "UCC_FPS_DEBUG" );
    return ( v && std::strcmp( v, "1" ) == 0 ) ? 1 : 0;
  }();
  return enabled == 1;
}

static void dbg(const char *msg)
{
  if ( !debug_enabled() || !msg )
    return;

  int fd = ::open("/tmp/ucc-layer-debug.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd >= 0) {
    ::write(fd, msg, strlen(msg));
    ::close(fd);
  }
}

#if defined(__GNUC__) || defined(__clang__)
#define UCC_LAYER_EXPORT __attribute__((visibility("default")))
#else
#define UCC_LAYER_EXPORT
#endif

// ============================================================================
//  Vulkan layer – dispatch table infrastructure
// ============================================================================

// MangoHud uses the HANDLE itself as the dispatch key because it guarantees
// stability regardless of whether the loader has overwritten the first pointer
// (the dispatch table).
static inline void *dispatch_key( void *handle )
{
  return handle;
}

struct DeviceDispatch
{
  PFN_vkGetDeviceProcAddr pfnGetDeviceProcAddr{};
  PFN_vkDestroyDevice     pfnDestroyDevice{};
  PFN_vkGetDeviceQueue    pfnGetDeviceQueue{};
  PFN_vkGetDeviceQueue2   pfnGetDeviceQueue2{};
  PFN_vkQueuePresentKHR   pfnQueuePresentKHR{};
  VkResult (VKAPI_PTR *pfnSetDeviceLoaderData)( VkDevice device, void *object ){};
};

struct QueueDispatch
{
  PFN_vkQueuePresentKHR pfnQueuePresentKHR{};
  void                 *deviceKey{};
  uint32_t              queueFamilyIndex{};
};

struct InstanceDispatch
{
  PFN_vkGetInstanceProcAddr pfnGetInstanceProcAddr{};
  PFN_vkDestroyInstance     pfnDestroyInstance{};
};

static std::unordered_map<void *, DeviceDispatch>   g_device_dispatch;
static std::unordered_map<void *, InstanceDispatch> g_instance_dispatch;
static std::unordered_map<void *, QueueDispatch>    g_queue_dispatch;
static std::shared_mutex                            g_lock;

// ============================================================================
//  Loader layer-interface structures (inlined to avoid dependency on
//  <vulkan/vk_layer.h>, which may not be installed on all systems).
// ============================================================================

// sType values used by the Vulkan loader when populating pNext chains.
static constexpr VkStructureType kLoaderInstanceCreateInfo =
  static_cast<VkStructureType>( 47 );
static constexpr VkStructureType kLoaderDeviceCreateInfo =
  static_cast<VkStructureType>( 48 );

enum VkLayerFunction_
{
  VK_LAYER_LINK_INFO      = 0,
  VK_LOADER_DATA_CALLBACK = 1,
};

struct VkLayerInstanceLink_
{
  VkLayerInstanceLink_     *pNext;
  PFN_vkGetInstanceProcAddr pfnNextGetInstanceProcAddr;
  PFN_vkGetInstanceProcAddr pfnThisGetInstanceProcAddr;
};

struct VkLayerDeviceLink_
{
  VkLayerDeviceLink_        *pNext;
  PFN_vkGetInstanceProcAddr  pfnNextGetInstanceProcAddr;
  PFN_vkGetDeviceProcAddr    pfnNextGetDeviceProcAddr;
};

struct VkLayerInstanceCreateInfo_
{
  VkStructureType sType;
  const void     *pNext;
  int             function;
  union
  {
    VkLayerInstanceLink_     *pLayerInfo;
    PFN_vkGetInstanceProcAddr pfnSetInstanceLoaderData;
  } u;
};

struct VkLayerDeviceCreateInfo_
{
  VkStructureType sType;
  const void     *pNext;
  int             function;
  union
  {
    VkLayerDeviceLink_ *pLayerInfo;
    void               *pfnSetDeviceLoaderData;  // unused by us
  } u;
};

static VkLayerInstanceCreateInfo_ *get_instance_chain( const VkInstanceCreateInfo *pCI, int function )
{
  auto *p = reinterpret_cast<const VkLayerInstanceCreateInfo_ *>( pCI->pNext );
  while ( p )
  {
    if ( p->sType == kLoaderInstanceCreateInfo && p->function == function )
      return const_cast<VkLayerInstanceCreateInfo_ *>( p );
    p = reinterpret_cast<const VkLayerInstanceCreateInfo_ *>( p->pNext );
  }
  return nullptr;
}

static VkLayerDeviceCreateInfo_ *get_device_chain( const VkDeviceCreateInfo *pCI, int function )
{
  auto *p = reinterpret_cast<const VkLayerDeviceCreateInfo_ *>( pCI->pNext );
  while ( p )
  {
    if ( p->sType == kLoaderDeviceCreateInfo && p->function == function )
      return const_cast<VkLayerDeviceCreateInfo_ *>( p );
    p = reinterpret_cast<const VkLayerDeviceCreateInfo_ *>( p->pNext );
  }
  return nullptr;
}

// ============================================================================
//  Intercepted Vulkan functions
// ============================================================================

static VKAPI_ATTR VkResult VKAPI_CALL
ucc_vkCreateInstance( const VkInstanceCreateInfo  *pCreateInfo,
                      const VkAllocationCallbacks *pAllocator,
                      VkInstance                  *pInstance )
{
  auto *chain = get_instance_chain( pCreateInfo, VK_LAYER_LINK_INFO );
  if ( !chain )
    return VK_ERROR_INITIALIZATION_FAILED;

  auto fpGIPA = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;  // advance to next layer

  auto fpCreate = reinterpret_cast<PFN_vkCreateInstance>(
    fpGIPA( VK_NULL_HANDLE, "vkCreateInstance" ) );
  if ( !fpCreate )
    return VK_ERROR_INITIALIZATION_FAILED;

  VkResult r = fpCreate( pCreateInfo, pAllocator, pInstance );
  if ( r != VK_SUCCESS )
    return r;

  auto fpDestroy = reinterpret_cast<PFN_vkDestroyInstance>(
    fpGIPA( *pInstance, "vkDestroyInstance" ) );

  {
    std::unique_lock lk( g_lock );
    auto &d                   = g_instance_dispatch[ dispatch_key( *pInstance ) ];
    d.pfnGetInstanceProcAddr  = fpGIPA;
    d.pfnDestroyInstance      = fpDestroy;
  }

  dbg("UCC: vkCreateInstance called\n");
  uccfps::ensure_reporter_started();
  return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
ucc_vkDestroyInstance( VkInstance instance, const VkAllocationCallbacks *pAllocator )
{
  void                  *key  = dispatch_key( instance );
  PFN_vkDestroyInstance  next = nullptr;
  {
    std::unique_lock lk( g_lock );
    auto it = g_instance_dispatch.find( key );
    if ( it != g_instance_dispatch.end() )
    {
      next = it->second.pfnDestroyInstance;
      g_instance_dispatch.erase( it );
    }
  }
  if ( next )
    next( instance, pAllocator );
}

static VKAPI_ATTR VkResult VKAPI_CALL
ucc_vkCreateDevice( VkPhysicalDevice             physDev,
                    const VkDeviceCreateInfo    *pCreateInfo,
                    const VkAllocationCallbacks *pAllocator,
                    VkDevice                    *pDevice )
{
  auto *chain = get_device_chain( pCreateInfo, VK_LAYER_LINK_INFO );
  auto *loadData = get_device_chain( pCreateInfo, VK_LOADER_DATA_CALLBACK );
  if ( !chain || !loadData )
    return VK_ERROR_INITIALIZATION_FAILED;

  // Both GPA variants come from the loader's chain link.
  auto fpGIPA = chain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  auto fpGDPA = chain->u.pLayerInfo->pfnNextGetDeviceProcAddr;
  chain->u.pLayerInfo = chain->u.pLayerInfo->pNext;

  // vkCreateDevice lives in the instance dispatch; NULL handle is valid here.
  auto fpCreate = reinterpret_cast<PFN_vkCreateDevice>(
    fpGIPA( VK_NULL_HANDLE, "vkCreateDevice" ) );
  if ( !fpCreate )
    return VK_ERROR_INITIALIZATION_FAILED;

  VkResult r = fpCreate( physDev, pCreateInfo, pAllocator, pDevice );
  if ( r != VK_SUCCESS )
    return r;

  // Log enabled device extensions
  dbg("UCC: Device extensions enabled:\n");
  for ( uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i ) {
    dbg("UCC:   "); dbg(pCreateInfo->ppEnabledExtensionNames[i]); dbg("\n");
  }

  auto fpDestroy = reinterpret_cast<PFN_vkDestroyDevice>(
    fpGDPA( *pDevice, "vkDestroyDevice" ) );
  auto fpGetDeviceQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(
    fpGDPA( *pDevice, "vkGetDeviceQueue" ) );
  auto fpGetDeviceQueue2 = reinterpret_cast<PFN_vkGetDeviceQueue2>(
    fpGDPA( *pDevice, "vkGetDeviceQueue2" ) );
  auto fpPresent = reinterpret_cast<PFN_vkQueuePresentKHR>(
    fpGDPA( *pDevice, "vkQueuePresentKHR" ) );

  // Pre-map queues declared at device creation time, so present interception
  // works even if the app never re-queries queue pointers later.
  if ( fpGetDeviceQueue && fpPresent )
  {
    const void *deviceKey = dispatch_key( *pDevice );
    for ( uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; ++i )
    {
      const uint32_t family = pCreateInfo->pQueueCreateInfos[i].queueFamilyIndex;
      for ( uint32_t j = 0; j < pCreateInfo->pQueueCreateInfos[i].queueCount; ++j )
      {
        VkQueue queue = VK_NULL_HANDLE;
        fpGetDeviceQueue( *pDevice, family, j, &queue );
        if ( !queue )
          continue;

        if ( loadData->u.pfnSetDeviceLoaderData )
        {
          auto setLoaderData = reinterpret_cast<VkResult (VKAPI_PTR *)( VkDevice, void * )>( loadData->u.pfnSetDeviceLoaderData );
          setLoaderData( *pDevice, queue );
        }

        std::unique_lock lk( g_lock );
        g_queue_dispatch[ dispatch_key( queue ) ] = QueueDispatch{ fpPresent, const_cast<void *>( deviceKey ), family };
      }
    }
  }

  {
    std::unique_lock lk( g_lock );
    auto &d               = g_device_dispatch[ dispatch_key( *pDevice ) ];
    d.pfnGetDeviceProcAddr = fpGDPA;
    d.pfnDestroyDevice     = fpDestroy;
    d.pfnGetDeviceQueue    = fpGetDeviceQueue;
    d.pfnGetDeviceQueue2   = fpGetDeviceQueue2;
    d.pfnQueuePresentKHR   = fpPresent;
    d.pfnSetDeviceLoaderData = reinterpret_cast<VkResult (VKAPI_PTR *)( VkDevice, void * )>( loadData->u.pfnSetDeviceLoaderData );
  }

  // Resolve physical-device memory properties for overlay staging buffer.
  {
    PFN_vkGetPhysicalDeviceMemoryProperties fpGetMemProps = nullptr;
    {
      std::shared_lock rlk( g_lock );
      for ( auto &[key, idisp] : g_instance_dispatch )
      {
        auto inst = reinterpret_cast<VkInstance>( key );
        fpGetMemProps = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
          idisp.pfnGetInstanceProcAddr( inst, "vkGetPhysicalDeviceMemoryProperties" ) );
        if ( fpGetMemProps )
          break;
      }
    }
    if ( fpGetMemProps )
    {
      VkPhysicalDeviceMemoryProperties memProps{};
      fpGetMemProps( physDev, &memProps );
      ucc_overlay::init_device( dispatch_key( *pDevice ), *pDevice, fpGDPA, memProps );
    }
  }

  dbg("UCC: vkCreateDevice OK\n");
  if ( fpPresent ) dbg("UCC:   next vkQueuePresentKHR found\n");
  else             dbg("UCC:   next vkQueuePresentKHR NULL\n");

  return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL
ucc_vkGetDeviceQueue( VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue *pQueue )
{
  dbg("UCC: ucc_vkGetDeviceQueue called\n");
  DeviceDispatch dispatch{};
  void *deviceKey = dispatch_key( device );
  {
    std::shared_lock lk( g_lock );
    auto it = g_device_dispatch.find( deviceKey );
    if ( it != g_device_dispatch.end() )
      dispatch = it->second;
  }

  if ( dispatch.pfnGetDeviceQueue )
    dispatch.pfnGetDeviceQueue( device, queueFamilyIndex, queueIndex, pQueue );

  if ( pQueue && *pQueue && dispatch.pfnSetDeviceLoaderData )
    dispatch.pfnSetDeviceLoaderData( device, *pQueue );

  if ( pQueue && *pQueue && dispatch.pfnQueuePresentKHR )
  {
    std::unique_lock lk( g_lock );
    g_queue_dispatch[ dispatch_key( *pQueue ) ] = QueueDispatch{ dispatch.pfnQueuePresentKHR, deviceKey, queueFamilyIndex };
  }
}

static VKAPI_ATTR void VKAPI_CALL
ucc_vkGetDeviceQueue2( VkDevice device, const VkDeviceQueueInfo2 *pQueueInfo, VkQueue *pQueue )
{
  dbg("UCC: ucc_vkGetDeviceQueue2 called\n");
  DeviceDispatch dispatch{};
  void *deviceKey = dispatch_key( device );
  {
    std::shared_lock lk( g_lock );
    auto it = g_device_dispatch.find( deviceKey );
    if ( it != g_device_dispatch.end() )
      dispatch = it->second;
  }

  if ( dispatch.pfnGetDeviceQueue2 )
    dispatch.pfnGetDeviceQueue2( device, pQueueInfo, pQueue );

  if ( pQueue && *pQueue && dispatch.pfnSetDeviceLoaderData )
    dispatch.pfnSetDeviceLoaderData( device, *pQueue );

  if ( pQueue && *pQueue && dispatch.pfnQueuePresentKHR )
  {
    std::unique_lock lk( g_lock );
    g_queue_dispatch[ dispatch_key( *pQueue ) ] = QueueDispatch{ dispatch.pfnQueuePresentKHR, deviceKey, pQueueInfo->queueFamilyIndex };
  }
}

static VKAPI_ATTR void VKAPI_CALL
ucc_vkDestroyDevice( VkDevice device, const VkAllocationCallbacks *pAllocator )
{
  void               *key  = dispatch_key( device );
  PFN_vkDestroyDevice next = nullptr;
  {
    std::unique_lock lk( g_lock );
    auto it = g_device_dispatch.find( key );
    if ( it != g_device_dispatch.end() )
    {
      next = it->second.pfnDestroyDevice;
      g_device_dispatch.erase( it );
    }

    for ( auto queueIt = g_queue_dispatch.begin(); queueIt != g_queue_dispatch.end(); )
    {
      if ( queueIt->second.deviceKey == key )
        queueIt = g_queue_dispatch.erase( queueIt );
      else
        ++queueIt;
    }
  }
  ucc_overlay::destroy_device( key );

  if ( next )
    next( device, pAllocator );
}

static VKAPI_ATTR VkResult VKAPI_CALL
ucc_vkQueuePresentKHR( VkQueue queue, const VkPresentInfoKHR *pPresentInfo )
{
  static bool once = false;
  if ( !once ) { once = true; dbg("UCC: vkQueuePresentKHR CALLED\n"); }
  uccfps::record_frame();

  PFN_vkQueuePresentKHR next = nullptr;
  void *key = dispatch_key( queue );
  void *devKey = nullptr;
  uint32_t queueFamily = 0;
  {
    std::shared_lock lk( g_lock );
    auto qit = g_queue_dispatch.find( key );
    if ( qit != g_queue_dispatch.end() )
    {
      next        = qit->second.pfnQueuePresentKHR;
      devKey      = qit->second.deviceKey;
      queueFamily = qit->second.queueFamilyIndex;
    }
    else
    {
      auto dit = g_device_dispatch.find( key );
      if ( dit != g_device_dispatch.end() )
        next = dit->second.pfnQueuePresentKHR;
    }
  }

  // Overlay blit (only active during Auto-OC / Undervolt scans)
  if ( devKey && pPresentInfo && pPresentInfo->swapchainCount > 0 )
  {
    ucc_overlay::before_present( queue, devKey, queueFamily,
                                 pPresentInfo->swapchainCount,
                                 pPresentInfo->pSwapchains,
                                 pPresentInfo->pImageIndices );
  }

  if ( !next ) dbg("UCC: vkQueuePresentKHR: no next found!\n");
  return next ? next( queue, pPresentInfo ) : VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL
ucc_vkCreateSwapchainKHR( VkDevice device,
                          const VkSwapchainCreateInfoKHR *pCreateInfo,
                          const VkAllocationCallbacks *pAllocator,
                          VkSwapchainKHR *pSwapchain )
{
  static bool once = false;
  if ( !once ) { once = true; dbg("UCC: vkCreateSwapchainKHR CALLED\n"); }

  PFN_vkCreateSwapchainKHR next = nullptr;
  {
    std::shared_lock lk( g_lock );
    auto it = g_device_dispatch.find( dispatch_key( device ) );
    if ( it != g_device_dispatch.end() && it->second.pfnGetDeviceProcAddr )
      next = reinterpret_cast<PFN_vkCreateSwapchainKHR>(
        it->second.pfnGetDeviceProcAddr( device, "vkCreateSwapchainKHR" ) );
  }
  if ( !next )
    return VK_ERROR_EXTENSION_NOT_PRESENT;

  // Add TRANSFER_DST so the overlay can blit onto swapchain images.
  VkSwapchainCreateInfoKHR modCI = *pCreateInfo;
  modCI.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;

  VkResult r = next( device, &modCI, pAllocator, pSwapchain );
  if ( r != VK_SUCCESS )
    r = next( device, pCreateInfo, pAllocator, pSwapchain ); // fallback without flag

  if ( r == VK_SUCCESS )
    ucc_overlay::register_swapchain( dispatch_key( device ), *pSwapchain,
                                     pCreateInfo->imageFormat,
                                     pCreateInfo->imageExtent.width,
                                     pCreateInfo->imageExtent.height );
  return r;
}

static VKAPI_ATTR void VKAPI_CALL
ucc_vkDestroySwapchainKHR( VkDevice device,
                           VkSwapchainKHR swapchain,
                           const VkAllocationCallbacks *pAllocator )
{
  PFN_vkDestroySwapchainKHR next = nullptr;
  {
    std::shared_lock lk( g_lock );
    auto it = g_device_dispatch.find( dispatch_key( device ) );
    if ( it != g_device_dispatch.end() && it->second.pfnGetDeviceProcAddr )
      next = reinterpret_cast<PFN_vkDestroySwapchainKHR>(
        it->second.pfnGetDeviceProcAddr( device, "vkDestroySwapchainKHR" ) );
  }
  ucc_overlay::unregister_swapchain( dispatch_key( device ), swapchain );
  if ( next ) next( device, swapchain, pAllocator );
}

static VKAPI_ATTR VkResult VKAPI_CALL
ucc_vkGetSwapchainImagesKHR( VkDevice device,
                             VkSwapchainKHR swapchain,
                             uint32_t *pCount,
                             VkImage *pSwapchainImages )
{
  PFN_vkGetSwapchainImagesKHR next = nullptr;
  {
    std::shared_lock lk( g_lock );
    auto it = g_device_dispatch.find( dispatch_key( device ) );
    if ( it != g_device_dispatch.end() && it->second.pfnGetDeviceProcAddr )
      next = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(
        it->second.pfnGetDeviceProcAddr( device, "vkGetSwapchainImagesKHR" ) );
  }
  if ( !next )
    return VK_ERROR_EXTENSION_NOT_PRESENT;

  VkResult r = next( device, swapchain, pCount, pSwapchainImages );
  if ( r == VK_SUCCESS && pSwapchainImages && pCount && *pCount > 0 )
    ucc_overlay::register_swapchain_images( dispatch_key( device ), swapchain,
                                            *pCount, pSwapchainImages );
  return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL
ucc_vkAcquireNextImageKHR( VkDevice device,
                           VkSwapchainKHR swapchain,
                           uint64_t timeout,
                           VkSemaphore semaphore,
                           VkFence fence,
                           uint32_t *pImageIndex )
{
  PFN_vkAcquireNextImageKHR next = nullptr;
  {
    std::shared_lock lk( g_lock );
    auto it = g_device_dispatch.find( dispatch_key( device ) );
    if ( it != g_device_dispatch.end() && it->second.pfnGetDeviceProcAddr )
      next = reinterpret_cast<PFN_vkAcquireNextImageKHR>(
        it->second.pfnGetDeviceProcAddr( device, "vkAcquireNextImageKHR" ) );
  }
  return next ? next( device, swapchain, timeout, semaphore, fence, pImageIndex ) : VK_ERROR_EXTENSION_NOT_PRESENT;
}

static VKAPI_ATTR VkResult VKAPI_CALL
ucc_vkAcquireNextImage2KHR( VkDevice device,
                            const VkAcquireNextImageInfoKHR *pAcquireInfo,
                            uint32_t *pImageIndex )
{
  PFN_vkAcquireNextImage2KHR next = nullptr;
  {
    std::shared_lock lk( g_lock );
    auto it = g_device_dispatch.find( dispatch_key( device ) );
    if ( it != g_device_dispatch.end() && it->second.pfnGetDeviceProcAddr )
      next = reinterpret_cast<PFN_vkAcquireNextImage2KHR>(
        it->second.pfnGetDeviceProcAddr( device, "vkAcquireNextImage2KHR" ) );
  }
  return next ? next( device, pAcquireInfo, pImageIndex ) : VK_ERROR_EXTENSION_NOT_PRESENT;
}

// ============================================================================
//  Entry-point tables
// ============================================================================

// Forward declarations (defined in extern "C" block below).
extern "C" {
UCC_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
VK_LAYER_UCC_FPS_GetInstanceProcAddr( VkInstance instance, const char *pName );
UCC_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
VK_LAYER_UCC_FPS_GetDeviceProcAddr( VkDevice device, const char *pName );
}

static PFN_vkVoidFunction get_instance_proc( const char *name )
{
  if ( !name ) return nullptr;
  if ( strcmp( name, "vkGetInstanceProcAddr" ) == 0 )
    return reinterpret_cast<PFN_vkVoidFunction>( VK_LAYER_UCC_FPS_GetInstanceProcAddr );
  if ( strcmp( name, "vkGetDeviceProcAddr" ) == 0 )
    return reinterpret_cast<PFN_vkVoidFunction>( VK_LAYER_UCC_FPS_GetDeviceProcAddr );
  if ( strcmp( name, "vkCreateInstance" ) == 0 )
    return reinterpret_cast<PFN_vkVoidFunction>( ucc_vkCreateInstance );
  if ( strcmp( name, "vkDestroyInstance" ) == 0 )
    return reinterpret_cast<PFN_vkVoidFunction>( ucc_vkDestroyInstance );
  if ( strcmp( name, "vkCreateDevice" ) == 0 )
    return reinterpret_cast<PFN_vkVoidFunction>( ucc_vkCreateDevice );
  return nullptr;
}

static PFN_vkVoidFunction get_device_proc( const char *name )
{
  if ( !name ) return nullptr;
  if ( strcmp( name, "vkGetDeviceProcAddr" ) == 0 )
    return reinterpret_cast<PFN_vkVoidFunction>( VK_LAYER_UCC_FPS_GetDeviceProcAddr );
  if ( strcmp( name, "vkDestroyDevice" ) == 0 )
    return reinterpret_cast<PFN_vkVoidFunction>( ucc_vkDestroyDevice );
  if ( !uccfps::hook_enabled() )
  {
    static bool once = false;
    if ( !once ) { once = true;
      dbg("UCC: HOOK DISABLED\n");
    }
    return nullptr;
  }
  if ( strcmp( name, "vkGetDeviceQueue" ) == 0 )
    return reinterpret_cast<PFN_vkVoidFunction>( ucc_vkGetDeviceQueue );
  if ( strcmp( name, "vkGetDeviceQueue2" ) == 0 )
    return reinterpret_cast<PFN_vkVoidFunction>( ucc_vkGetDeviceQueue2 );
  if ( strcmp( name, "vkCreateSwapchainKHR" ) == 0 )
    return reinterpret_cast<PFN_vkVoidFunction>( ucc_vkCreateSwapchainKHR );
  if ( strcmp( name, "vkDestroySwapchainKHR" ) == 0 )
    return reinterpret_cast<PFN_vkVoidFunction>( ucc_vkDestroySwapchainKHR );
  if ( strcmp( name, "vkGetSwapchainImagesKHR" ) == 0 )
    return reinterpret_cast<PFN_vkVoidFunction>( ucc_vkGetSwapchainImagesKHR );
  if ( strcmp( name, "vkAcquireNextImageKHR" ) == 0 )
    return reinterpret_cast<PFN_vkVoidFunction>( ucc_vkAcquireNextImageKHR );
  if ( strcmp( name, "vkAcquireNextImage2KHR" ) == 0 )
    return reinterpret_cast<PFN_vkVoidFunction>( ucc_vkAcquireNextImage2KHR );
  if ( strcmp( name, "vkQueuePresentKHR" ) == 0 )
  {
    static bool once2 = false;
    if ( !once2 ) { once2 = true; dbg("UCC: Present HOOK registered\n"); }
    return reinterpret_cast<PFN_vkVoidFunction>( ucc_vkQueuePresentKHR );
  }
  return nullptr;
}

// ============================================================================
//  Exported layer entry points + VkNegotiateLayerInterface
// ============================================================================

// VkNegotiateLayerInterface (inlined; avoids dependency on vk_layer.h).
enum VkNegotiateLayerStructType_
{
  LAYER_NEGOTIATE_INTERFACE_STRUCT = 1,
};
struct VkNegotiateLayerInterface_
{
  VkNegotiateLayerStructType_ sType;
  void                       *pNext;
  uint32_t                    loaderLayerInterfaceVersion;
  PFN_vkGetInstanceProcAddr   pfnGetInstanceProcAddr;
  PFN_vkGetDeviceProcAddr     pfnGetDeviceProcAddr;
  PFN_vkGetInstanceProcAddr   pfnGetPhysicalDeviceProcAddr;
};

extern "C" {

// Enumeration intercepts removed to avoid modifying loader behavior.

UCC_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr( VkInstance instance, const char *pName )
{
  return VK_LAYER_UCC_FPS_GetInstanceProcAddr( instance, pName );
}

UCC_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr( VkDevice device, const char *pName )
{
  return VK_LAYER_UCC_FPS_GetDeviceProcAddr( device, pName );
}

UCC_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
VK_LAYER_UCC_FPS_GetInstanceProcAddr( VkInstance instance, const char *pName )
{
  auto fn = get_instance_proc( pName );
  if ( fn ) return fn;
  fn = get_device_proc( pName );
  if ( fn ) return fn;

  if ( instance == VK_NULL_HANDLE ) return nullptr;
  std::shared_lock lk( g_lock );
  auto it = g_instance_dispatch.find( dispatch_key( instance ) );
  if ( it == g_instance_dispatch.end() ) return nullptr;
  return it->second.pfnGetInstanceProcAddr( instance, pName );
}

UCC_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
VK_LAYER_UCC_FPS_GetDeviceProcAddr( VkDevice device, const char *pName )
{
  auto fn = get_device_proc( pName );
  if ( fn ) return fn;

  if ( device == VK_NULL_HANDLE ) return nullptr;
  std::shared_lock lk( g_lock );
  auto it = g_device_dispatch.find( dispatch_key( device ) );
  if ( it == g_device_dispatch.end() ) return nullptr;
  return it->second.pfnGetDeviceProcAddr( device, pName );
}

// Standard-name exports so the Vulkan loader can discover intercepted functions
// via dlsym() on the layer DSO (in addition to the GDPA path).
UCC_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL
vkGetDeviceQueue( VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue *pQueue )
{
  ucc_vkGetDeviceQueue( device, queueFamilyIndex, queueIndex, pQueue );
}

UCC_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL
vkGetDeviceQueue2( VkDevice device, const VkDeviceQueueInfo2 *pQueueInfo, VkQueue *pQueue )
{
  ucc_vkGetDeviceQueue2( device, pQueueInfo, pQueue );
}

UCC_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL
vkDestroyDevice( VkDevice device, const VkAllocationCallbacks *pAllocator )
{
  ucc_vkDestroyDevice( device, pAllocator );
}

UCC_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkCreateSwapchainKHR( VkDevice device,
                      const VkSwapchainCreateInfoKHR *pCreateInfo,
                      const VkAllocationCallbacks *pAllocator,
                      VkSwapchainKHR *pSwapchain )
{
  return ucc_vkCreateSwapchainKHR( device, pCreateInfo, pAllocator, pSwapchain );
}

UCC_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL
vkDestroySwapchainKHR( VkDevice device,
                       VkSwapchainKHR swapchain,
                       const VkAllocationCallbacks *pAllocator )
{
  ucc_vkDestroySwapchainKHR( device, swapchain, pAllocator );
}

UCC_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkGetSwapchainImagesKHR( VkDevice device,
                         VkSwapchainKHR swapchain,
                         uint32_t *pCount,
                         VkImage *pSwapchainImages )
{
  return ucc_vkGetSwapchainImagesKHR( device, swapchain, pCount, pSwapchainImages );
}

UCC_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkAcquireNextImageKHR( VkDevice device,
                       VkSwapchainKHR swapchain,
                       uint64_t timeout,
                       VkSemaphore semaphore,
                       VkFence fence,
                       uint32_t *pImageIndex )
{
  return ucc_vkAcquireNextImageKHR( device, swapchain, timeout, semaphore, fence, pImageIndex );
}

UCC_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkAcquireNextImage2KHR( VkDevice device,
                        const VkAcquireNextImageInfoKHR *pAcquireInfo,
                        uint32_t *pImageIndex )
{
  return ucc_vkAcquireNextImage2KHR( device, pAcquireInfo, pImageIndex );
}

UCC_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkQueuePresentKHR( VkQueue queue, const VkPresentInfoKHR *pPresentInfo )
{
  return ucc_vkQueuePresentKHR( queue, pPresentInfo );
}

UCC_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion( VkNegotiateLayerInterface_ *pVer )
{
  if ( pVer->loaderLayerInterfaceVersion > 2 )
    pVer->loaderLayerInterfaceVersion = 2;
  pVer->pfnGetInstanceProcAddr       = VK_LAYER_UCC_FPS_GetInstanceProcAddr;
  pVer->pfnGetDeviceProcAddr         = VK_LAYER_UCC_FPS_GetDeviceProcAddr;
  pVer->pfnGetPhysicalDeviceProcAddr = nullptr;
  return VK_SUCCESS;
}

}  // extern "C"
