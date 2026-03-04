// SPDX-License-Identifier: GPL-3.0-or-later
//
// ucc-fps-layer/src/layer.cpp
//
// Minimal Vulkan implicit layer + GLX LD_PRELOAD hook that counts presented
// frames and streams rolling FPS to uccd's FPS socket whenever the socket
// exists (i.e. an Auto-OC scan is in progress).
//
// Vulkan:  loaded automatically by the Vulkan loader as an implicit layer.
// OpenGL:  the same .so can be LD_PRELOADed for GLX games; it hooks
//          glXSwapBuffers() via RTLD_NEXT.

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <array>
#include <cctype>
#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(__GNUC__) || defined(__clang__)
#define UCC_LAYER_EXPORT __attribute__((visibility("default")))
#else
#define UCC_LAYER_EXPORT
#endif

// ============================================================================
//  FPS reporter – one background thread shared across Vulkan + GLX paths
// ============================================================================

static constexpr const char *kSocketPathPrimary = "/run/ucc/uccd-fps.sock";
static constexpr const char *kSocketPathLegacy  = "/tmp/uccd-fps.sock";

static std::vector< std::string > socket_paths()
{
  std::vector< std::string > paths;
  if ( const char *envPath = ::getenv( "UCC_FPS_SOCKET" ); envPath && envPath[0] != '\0' )
    paths.emplace_back( envPath );
  paths.emplace_back( kSocketPathPrimary );
  paths.emplace_back( kSocketPathLegacy );
  return paths;
}

static std::string to_lower_copy( std::string s )
{
  for ( char &ch : s )
    ch = static_cast< char >( std::tolower( static_cast< unsigned char >( ch ) ) );
  return s;
}

static std::string get_self_comm()
{
  std::array< char, 256 > buf{};
  FILE *f = ::fopen( "/proc/self/comm", "r" );
  if ( !f )
    return {};
  const size_t n = ::fread( buf.data(), 1, buf.size() - 1, f );
  ::fclose( f );
  if ( n == 0 )
    return {};
  std::string s( buf.data(), n );
  while ( !s.empty() && ( s.back() == '\n' || s.back() == '\r' || s.back() == ' ' ) )
    s.pop_back();
  return s;
}

static std::string get_self_exe_basename()
{
  std::array< char, 512 > path{};
  const ssize_t n = ::readlink( "/proc/self/exe", path.data(), path.size() - 1 );
  if ( n <= 0 )
    return {};
  path[ static_cast< size_t >( n ) ] = '\0';
  const char *base = ::strrchr( path.data(), '/' );
  return base ? std::string( base + 1 ) : std::string( path.data() );
}

static bool should_enable_layer_for_process()
{
  // Manual override for debugging.
  if ( const char *force = ::getenv( "UCC_FPS_FORCE_ENABLE" ); force && strcmp( force, "1" ) == 0 )
    return true;

  const std::string comm = to_lower_copy( get_self_comm() );
  const std::string exe  = to_lower_copy( get_self_exe_basename() );

  // Known launchers that are unstable with the implicit layer during startup.
  static constexpr std::array< const char *, 6 > kBlocked = {
    "steam",
    "steamwebhelper",
    "steamservice",
    "heroic",
    "heroicgameslauncher",
    "heroic-browser",
  };

  for ( const char *name : kBlocked )
  {
    if ( comm == name || exe == name )
      return false;
  }

  return true;
}

static bool layer_enabled()
{
  static const bool enabled = should_enable_layer_for_process();
  return enabled;
}

/// Incremented on every present/swap call (both Vulkan and GLX paths).
static std::atomic<uint64_t> g_frame_count{ 0 };

static void fps_reporter_thread()
{
  int sock_fd = -1;

  uint64_t last_count  = 0;
  auto     last_time   = std::chrono::steady_clock::now();

  const auto interval = std::chrono::milliseconds( 500 );

  while ( true )
  {
    std::this_thread::sleep_for( interval );

    auto     now   = std::chrono::steady_clock::now();
    uint64_t count = g_frame_count.load( std::memory_order_relaxed );
    double   dt    = std::chrono::duration<double>( now - last_time ).count();
    double   fps   = ( count - last_count ) / dt;
    last_count     = count;
    last_time      = now;

    // Attempt (re)connect when the socket file is present
    if ( sock_fd < 0 )
    {
      for ( const std::string &path : socket_paths() )
      {
        int fd = ::socket( AF_UNIX, SOCK_STREAM, 0 );
        if ( fd < 0 )
          continue;

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        ::strncpy( addr.sun_path, path.c_str(), sizeof( addr.sun_path ) - 1 );

        if ( ::connect( fd, reinterpret_cast<sockaddr *>( &addr ), sizeof( addr ) ) == 0 )
        {
          sock_fd = fd;
          break;
        }
        else
          ::close( fd );
      }
    }

    if ( sock_fd >= 0 )
    {
      char buf[ 32 ];
      int  n = ::snprintf( buf, sizeof( buf ), "fps:%.1f\n", fps );
      if ( n > 0 && ::write( sock_fd, buf, static_cast<size_t>( n ) ) < 0 )
      {
        ::close( sock_fd );
        sock_fd = -1;
      }
    }
  }
}

static void ensure_reporter_started()
{
  if ( !layer_enabled() )
    return;

  static std::once_flag s_started;
  std::call_once( s_started, []() {
    std::thread t( fps_reporter_thread );
    t.detach();
  } );
}

// ============================================================================
//  Vulkan layer – dispatch table infrastructure
// ============================================================================

/// The dispatch key is the first pointer embedded in every dispatchable handle.
static inline void *dispatch_key( void *handle )
{
  return *reinterpret_cast<void **>( handle );
}

struct DeviceDispatch
{
  PFN_vkGetDeviceProcAddr pfnGetDeviceProcAddr{};
  PFN_vkDestroyDevice     pfnDestroyDevice{};
  PFN_vkQueuePresentKHR   pfnQueuePresentKHR{};
};

struct InstanceDispatch
{
  PFN_vkGetInstanceProcAddr pfnGetInstanceProcAddr{};
  PFN_vkDestroyInstance     pfnDestroyInstance{};
};

static std::unordered_map<void *, DeviceDispatch>   g_device_dispatch;
static std::unordered_map<void *, InstanceDispatch> g_instance_dispatch;
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
  VK_LAYER_LINK_INFO = 0,
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

static VkLayerInstanceCreateInfo_ *get_instance_chain( const VkInstanceCreateInfo *pCI )
{
  auto *p = reinterpret_cast<const VkLayerInstanceCreateInfo_ *>( pCI->pNext );
  while ( p )
  {
    if ( p->sType == kLoaderInstanceCreateInfo && p->function == VK_LAYER_LINK_INFO )
      return const_cast<VkLayerInstanceCreateInfo_ *>( p );
    p = reinterpret_cast<const VkLayerInstanceCreateInfo_ *>( p->pNext );
  }
  return nullptr;
}

static VkLayerDeviceCreateInfo_ *get_device_chain( const VkDeviceCreateInfo *pCI )
{
  auto *p = reinterpret_cast<const VkLayerDeviceCreateInfo_ *>( pCI->pNext );
  while ( p )
  {
    if ( p->sType == kLoaderDeviceCreateInfo && p->function == VK_LAYER_LINK_INFO )
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
  auto *chain = get_instance_chain( pCreateInfo );
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

  ensure_reporter_started();
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
  auto *chain = get_device_chain( pCreateInfo );
  if ( !chain )
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

  auto fpDestroy = reinterpret_cast<PFN_vkDestroyDevice>(
    fpGDPA( *pDevice, "vkDestroyDevice" ) );
  auto fpPresent = reinterpret_cast<PFN_vkQueuePresentKHR>(
    fpGDPA( *pDevice, "vkQueuePresentKHR" ) );

  {
    std::unique_lock lk( g_lock );
    auto &d               = g_device_dispatch[ dispatch_key( *pDevice ) ];
    d.pfnGetDeviceProcAddr = fpGDPA;
    d.pfnDestroyDevice     = fpDestroy;
    d.pfnQueuePresentKHR   = fpPresent;
  }

  return VK_SUCCESS;
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
  }
  if ( next )
    next( device, pAllocator );
}

static VKAPI_ATTR VkResult VKAPI_CALL
ucc_vkQueuePresentKHR( VkQueue queue, const VkPresentInfoKHR *pPresentInfo )
{
  if ( layer_enabled() )
    g_frame_count.fetch_add( 1, std::memory_order_relaxed );

  PFN_vkQueuePresentKHR next = nullptr;
  {
    std::shared_lock lk( g_lock );
    auto it = g_device_dispatch.find( dispatch_key( queue ) );
    if ( it != g_device_dispatch.end() )
      next = it->second.pfnQueuePresentKHR;
  }
  return next ? next( queue, pPresentInfo ) : VK_SUCCESS;
}

// ============================================================================
//  Entry-point tables
// ============================================================================

static PFN_vkVoidFunction get_instance_proc( const char *name )
{
  if ( !name ) return nullptr;
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
  if ( strcmp( name, "vkDestroyDevice" ) == 0 )
    return reinterpret_cast<PFN_vkVoidFunction>( ucc_vkDestroyDevice );
  if ( strcmp( name, "vkQueuePresentKHR" ) == 0 )
    return reinterpret_cast<PFN_vkVoidFunction>( ucc_vkQueuePresentKHR );
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

// Some loaders look for these names directly.
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

// ============================================================================
//  GLX hook – activated via LD_PRELOAD for OpenGL games
// ============================================================================

/// Signature of glXSwapBuffers as specified by the GLX spec.
using glXSwapBuffers_t = void (*)( void * /*Display* */, unsigned long /*GLXDrawable*/ );

void glXSwapBuffers( void *dpy, unsigned long drawable )
{
  if ( layer_enabled() )
  {
    g_frame_count.fetch_add( 1, std::memory_order_relaxed );
    ensure_reporter_started();
  }

  static glXSwapBuffers_t s_real = nullptr;
  static std::once_flag   s_flag;
  std::call_once( s_flag, []() {
    s_real = reinterpret_cast<glXSwapBuffers_t>( ::dlsym( RTLD_NEXT, "glXSwapBuffers" ) );
  } );

  if ( s_real )
    s_real( dpy, drawable );
}

}  // extern "C"
