/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "NvmlWrapper.hpp"
#include <algorithm>
#include <cstring>

namespace
{
constexpr uint32_t NVAPI_INITIALIZE_ID = 0x0150E828;
constexpr uint32_t NVAPI_UNLOAD_ID = 0xD22BDD7E;
constexpr uint32_t NVAPI_ENUM_PHYSICAL_GPUS_ID = 0xE5AC921F;
constexpr uint32_t NVAPI_VOLTAGE_ID = 0x465F9BCF;
constexpr uint32_t NVAPI_OK = 0;
constexpr nvml::nvmlReturn_t NVML_ERROR_NO_PERMISSION = 4;

inline bool isExpectedOcWriteRejection( nvml::nvmlReturn_t ret )
{
  return ret == nvml::NVML_ERROR_NOT_SUPPORTED || ret == NVML_ERROR_NO_PERMISSION;
}

inline int offsetKey( nvml::nvmlClockType_t clockType, nvml::nvmlPstates_t pstate )
{
  return static_cast< int >( clockType ) * 100 + static_cast< int >( pstate );
}
}

NvmlWrapper::NvmlWrapper( bool enableOcFeatures )
  : m_enableOcFeatures( enableOcFeatures )
{
  // Try to load the NVML library
  m_lib = dlopen( "libnvidia-ml.so.1", RTLD_LAZY | RTLD_LOCAL );
  if ( !m_lib )
  {
    std::cerr << "[NvmlWrapper] Could not load libnvidia-ml.so.1: " << dlerror() << std::endl;
    return;
  }

  // Load required function pointers
  m_init = loadSym< InitFn >( "nvmlInit_v2" );
  m_shutdown = loadSym< ShutdownFn >( "nvmlShutdown" );
  m_getCount = loadSym< DeviceGetCountFn >( "nvmlDeviceGetCount_v2" );
  m_getHandle = loadSym< DeviceGetHandleByIndexFn >( "nvmlDeviceGetHandleByIndex_v2" );
  m_getName = loadSym< DeviceGetNameFn >( "nvmlDeviceGetName" );
  m_getPciBusId = loadSym< DeviceGetPciBusIdFn >( "nvmlDeviceGetPciInfo_v3" );
  m_getTemperature = loadSym< DeviceGetTemperatureFn >( "nvmlDeviceGetTemperature" );
  m_getTemperatureThreshold = loadSym< DeviceGetTemperatureThresholdFn >( "nvmlDeviceGetTemperatureThreshold" );
  m_getPowerUsage = loadSym< DeviceGetPowerUsageFn >( "nvmlDeviceGetPowerUsage" );
  m_getPowerLimit = loadSym< DeviceGetPowerManagementLimitFn >( "nvmlDeviceGetPowerManagementLimit" );
  m_setPowerLimit = loadSym< DeviceSetPowerManagementLimitFn >( "nvmlDeviceSetPowerManagementLimit" );
  m_getPowerLimitConstraints = loadSym< DeviceGetPowerManagementLimitConstraintsFn >( "nvmlDeviceGetPowerManagementLimitConstraints" );
  m_getPowerLimitDefault = loadSym< DeviceGetPowerManagementDefaultLimitFn >( "nvmlDeviceGetPowerManagementDefaultLimit" );
  m_getPerformanceState = loadSym< DeviceGetPerformanceStateFn >( "nvmlDeviceGetPerformanceState" );
  m_getClockInfo = loadSym< DeviceGetClockInfoFn >( "nvmlDeviceGetClockInfo" );
  m_getMaxClockInfo = loadSym< DeviceGetMaxClockInfoFn >( "nvmlDeviceGetMaxClockInfo" );
  m_getEnforcedPowerLimit = loadSym< DeviceGetEnforcedPowerLimitFn >( "nvmlDeviceGetEnforcedPowerLimit" );
  m_getUtilizationRates   = loadSym< DeviceGetUtilizationRatesFn >( "nvmlDeviceGetUtilizationRates" );
  m_getMemoryInfo = loadSym< DeviceGetMemoryInfoFn >( "nvmlDeviceGetMemoryInfo_v2" );
  if ( !m_getMemoryInfo )
    m_getMemoryInfo = loadSym< DeviceGetMemoryInfoFn >( "nvmlDeviceGetMemoryInfo" );
  m_getCurrentClocksThrottleReasons = loadSym< DeviceGetCurrentClocksThrottleReasonsFn >( "nvmlDeviceGetCurrentClocksThrottleReasons" );
  m_getEncoderUtilization = loadSym< DeviceGetEncoderUtilizationFn >( "nvmlDeviceGetEncoderUtilization" );
  m_getDecoderUtilization = loadSym< DeviceGetDecoderUtilizationFn >( "nvmlDeviceGetDecoderUtilization" );

  // OC-specific functions (may not exist on older drivers)
  m_getSupportedPstates = loadSym< DeviceGetSupportedPstatesFn >( "nvmlDeviceGetSupportedPerformanceStates" );
  m_getMinMaxClock = loadSym< DeviceGetMinMaxClockFn >( "nvmlDeviceGetMinMaxClockOfPState" );
  m_getClockOffsets = loadSym< DeviceGetClockOffsetsFn >( "nvmlDeviceGetClockOffsets" );
  m_setClockOffsets = loadSym< DeviceSetClockOffsetsFn >( "nvmlDeviceSetClockOffsets" );
  m_setGpuLockedClocks = loadSym< DeviceSetGpuLockedClocksFn >( "nvmlDeviceSetGpuLockedClocks" );
  m_setMemLockedClocks = loadSym< DeviceSetMemLockedClocksFn >( "nvmlDeviceSetMemoryLockedClocks" );
  m_resetGpuLockedClocks = loadSym< DeviceResetGpuLockedClocksFn >( "nvmlDeviceResetGpuLockedClocks" );
  m_resetMemLockedClocks = loadSym< DeviceResetMemLockedClocksFn >( "nvmlDeviceResetMemoryLockedClocks" );

  // Check minimum required functions
  if ( !m_init || !m_shutdown || !m_getCount || !m_getHandle )
  {
    std::cerr << "[NvmlWrapper] Missing critical NVML symbols" << std::endl;
    dlclose( m_lib );
    m_lib = nullptr;
    return;
  }

  // Initialize NVML
  nvml::nvmlReturn_t ret = m_init();
  if ( ret != nvml::NVML_SUCCESS )
  {
    std::cerr << "[NvmlWrapper] nvmlInit failed with code " << ret << std::endl;
    dlclose( m_lib );
    m_lib = nullptr;
    return;
  }

  // Get device count
  ret = m_getCount( &m_deviceCount );
  if ( ret != nvml::NVML_SUCCESS )
  {
    std::cerr << "[NvmlWrapper] nvmlDeviceGetCount failed with code " << ret << std::endl;
    m_shutdown();
    dlclose( m_lib );
    m_lib = nullptr;
    return;
  }

  m_initialized = true;
  std::cerr << "[NvmlWrapper] Initialized successfully, found " << m_deviceCount << " GPU(s)" << std::endl;

  if ( m_enableOcFeatures )
  {
    cacheSupportedPstates();
    probeWritableOffsetPstates();
  }
  initNvapi();
}

void NvmlWrapper::cacheSupportedPstates()
{
  if ( !m_getSupportedPstates )
    return;

  for ( unsigned int deviceIndex = 0; deviceIndex < m_deviceCount; ++deviceIndex )
  {
    auto devOpt = getDevice( deviceIndex );
    if ( !devOpt )
      continue;

    nvml::nvmlPstates_t pstateArr[nvml::NVML_MAX_GPU_PERF_PSTATES];
    std::memset( pstateArr, 0xFF, sizeof( pstateArr ) );

    if ( m_getSupportedPstates( *devOpt, pstateArr, nvml::NVML_MAX_GPU_PERF_PSTATES )
         != nvml::NVML_SUCCESS )
      continue;

    auto &cached = m_supportedPstates[deviceIndex];
    cached.clear();
    for ( unsigned int i = 0; i < nvml::NVML_MAX_GPU_PERF_PSTATES; ++i )
    {
      if ( pstateArr[i] == nvml::NVML_PSTATE_UNKNOWN )
        break;
      cached.push_back( pstateArr[i] );
    }

    std::cerr << "[NvmlWrapper] Found " << cached.size()
              << " supported P-state(s) on GPU " << deviceIndex << std::endl;
  }
}

void NvmlWrapper::probeWritableOffsetPstates()
{
  if ( !m_getClockOffsets || !m_setClockOffsets )
    return;

  for ( unsigned int deviceIndex = 0; deviceIndex < m_deviceCount; ++deviceIndex )
  {
    auto devOpt = getDevice( deviceIndex );
    if ( !devOpt )
      continue;

    auto device = *devOpt;

    auto pstatesIt = m_supportedPstates.find( deviceIndex );
    if ( pstatesIt == m_supportedPstates.end() || pstatesIt->second.empty() )
      continue;

    for ( auto pstate : pstatesIt->second )
    {
      for ( auto clockType : { nvml::NVML_CLOCK_GRAPHICS, nvml::NVML_CLOCK_MEM } )
      {
        nvml::nvmlClockOffset_t info{};
        info.version = NVML_CLOCK_OFFSET_VER1;
        info.type = clockType;
        info.pstate = pstate;

        if ( m_getClockOffsets( device, &info ) != nvml::NVML_SUCCESS )
        {
          m_writableOffsets[deviceIndex][offsetKey( clockType, pstate )] = false;
          continue;
        }

        const int currentOffset = info.clockOffsetMHz;
        nvml::nvmlClockOffset_t writeInfo{};
        writeInfo.version = NVML_CLOCK_OFFSET_VER1;
        writeInfo.type = clockType;
        writeInfo.pstate = pstate;
        writeInfo.clockOffsetMHz = currentOffset;

        const auto ret = m_setClockOffsets( device, &writeInfo );
        m_writableOffsets[deviceIndex][offsetKey( clockType, pstate )] = ( ret == nvml::NVML_SUCCESS );
      }
    }
  }
}

NvmlWrapper::~NvmlWrapper()
{
  if ( m_initialized && m_shutdown )
  {
    m_shutdown();
  }
  if ( m_lib )
  {
    dlclose( m_lib );
  }

  if ( m_nvapiInitialized && m_nvapiUnload )
  {
    m_nvapiUnload();
  }
  if ( m_nvapiLib )
  {
    dlclose( m_nvapiLib );
  }
}

void NvmlWrapper::initNvapi()
{
  m_nvapiLib = dlopen( "libnvidia-api.so.1", RTLD_LAZY | RTLD_LOCAL );
  if ( !m_nvapiLib )
    return;

  m_nvapiQueryInterface = reinterpret_cast< NvApiQueryInterfaceFn >( dlsym( m_nvapiLib, "nvapi_QueryInterface" ) );
  if ( !m_nvapiQueryInterface )
    return;

  m_nvapiInitialize = reinterpret_cast< NvApiInitializeFn >( m_nvapiQueryInterface( NVAPI_INITIALIZE_ID ) );
  m_nvapiUnload = reinterpret_cast< NvApiUnloadFn >( m_nvapiQueryInterface( NVAPI_UNLOAD_ID ) );
  m_nvapiEnumPhysicalGpus = reinterpret_cast< NvApiEnumPhysicalGPUsFn >( m_nvapiQueryInterface( NVAPI_ENUM_PHYSICAL_GPUS_ID ) );
  m_nvapiGetVoltage = reinterpret_cast< NvApiGetVoltageFn >( m_nvapiQueryInterface( NVAPI_VOLTAGE_ID ) );

  if ( !m_nvapiInitialize || !m_nvapiEnumPhysicalGpus || !m_nvapiGetVoltage )
    return;

  if ( m_nvapiInitialize() != static_cast< int32_t >( NVAPI_OK ) )
    return;

  void *gpuHandles[64] = {};
  uint32_t count = 0;
  if ( m_nvapiEnumPhysicalGpus( gpuHandles, &count ) != static_cast< int32_t >( NVAPI_OK ) || count == 0 )
    return;

  m_nvapiGpuHandles.assign( gpuHandles, gpuHandles + count );
  m_nvapiInitialized = true;
}

std::optional< nvml::nvmlDevice_t > NvmlWrapper::getDevice( unsigned int index ) const
{
  if ( !m_initialized || !m_getHandle || index >= m_deviceCount )
    return std::nullopt;

  nvml::nvmlDevice_t device = nullptr;
  if ( m_getHandle( index, &device ) != nvml::NVML_SUCCESS )
    return std::nullopt;

  return device;
}

std::optional< NvmlOCState > NvmlWrapper::getOCState( unsigned int deviceIndex ) const
{
  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt )
    return std::nullopt;

  auto device = *devOpt;
  NvmlOCState state;

  // GPU name
  if ( m_getName )
  {
    char name[256] = {};
    if ( m_getName( device, name, sizeof( name ) ) == nvml::NVML_SUCCESS )
      state.gpuName = name;
  }

  // Temperature
  if ( m_getTemperature )
  {
    unsigned int temp = 0;
    if ( m_getTemperature( device, 0 /* GPU */, &temp ) == nvml::NVML_SUCCESS )
      state.tempC = temp;
  }
  if ( m_getTemperatureThreshold )
  {
    unsigned int threshold = 0;
    if ( m_getTemperatureThreshold( device, 0 /* Shutdown */, &threshold ) == nvml::NVML_SUCCESS )
      state.tempShutdownC = threshold;
  }

  // Power info
  if ( m_getPowerUsage )
  {
    unsigned int mw = 0;
    if ( m_getPowerUsage( device, &mw ) == nvml::NVML_SUCCESS )
      state.powerDrawW = mw / 1000.0;
  }
  if ( m_getPowerLimit )
  {
    unsigned int mw = 0;
    if ( m_getPowerLimit( device, &mw ) == nvml::NVML_SUCCESS )
      state.powerLimitW = mw / 1000.0;
  }
  if ( m_getPowerLimitDefault )
  {
    unsigned int mw = 0;
    if ( m_getPowerLimitDefault( device, &mw ) == nvml::NVML_SUCCESS )
      state.powerDefaultW = mw / 1000.0;
  }
  if ( m_getPowerLimitConstraints )
  {
    unsigned int minMw = 0, maxMw = 0;
    if ( m_getPowerLimitConstraints( device, &minMw, &maxMw ) == nvml::NVML_SUCCESS )
    {
      state.powerMinW = minMw / 1000.0;
      state.powerMaxW = maxMw / 1000.0;
    }
  }

  std::vector< nvml::nvmlPstates_t > pstates;
  if ( auto it = m_supportedPstates.find( deviceIndex ); it != m_supportedPstates.end() )
    pstates = it->second;

  // Read per-pstate clocks and offsets
  unsigned int gpuMinAll = UINT32_MAX, gpuMaxAll = 0;
  unsigned int vramMinAll = UINT32_MAX, vramMaxAll = 0;

  for ( auto ps : pstates )
  {
    // GPU clocks
    if ( m_getMinMaxClock )
    {
      unsigned int pMin = 0, pMax = 0;
      if ( m_getMinMaxClock( device, nvml::NVML_CLOCK_GRAPHICS, ps, &pMin, &pMax ) == nvml::NVML_SUCCESS )
      {
        NvmlPStateClockInfo info{};
        info.pstate = static_cast< unsigned int >( ps );
        info.minMHz = pMin;
        info.maxMHz = pMax;

        // Clock offset
        if ( m_getClockOffsets )
        {
          nvml::nvmlClockOffset_t offsetInfo{};
          offsetInfo.version = NVML_CLOCK_OFFSET_VER1;
          offsetInfo.type = nvml::NVML_CLOCK_GRAPHICS;
          offsetInfo.pstate = ps;
          if ( m_getClockOffsets( device, &offsetInfo ) == nvml::NVML_SUCCESS )
          {
            info.currentOffset = offsetInfo.clockOffsetMHz;
            info.minOffset = offsetInfo.minClockOffsetMHz;
            info.maxOffset = offsetInfo.maxClockOffsetMHz;
            state.offsetsSupported = true;
          }
        }

        // Check for locally tracked offset if driver doesn't report
        int key = offsetKey( nvml::NVML_CLOCK_GRAPHICS, ps );
        if ( info.currentOffset == 0 )
        {
          auto devIt = m_appliedOffsets.find( deviceIndex );
          if ( devIt != m_appliedOffsets.end() )
          {
            auto offIt = devIt->second.find( key );
            if ( offIt != devIt->second.end() )
              info.currentOffset = offIt->second;
          }
        }

        {
          auto devIt = m_writableOffsets.find( deviceIndex );
          if ( devIt != m_writableOffsets.end() )
          {
            auto writableIt = devIt->second.find( key );
            if ( writableIt != devIt->second.end() )
              info.offsetWritable = writableIt->second;
          }
        }

        state.gpuPStates.push_back( info );
        gpuMinAll = std::min( gpuMinAll, pMin );
        gpuMaxAll = std::max( gpuMaxAll, pMax );
      }
    }

    // VRAM clocks
    if ( m_getMinMaxClock )
    {
      unsigned int pMin = 0, pMax = 0;
      if ( m_getMinMaxClock( device, nvml::NVML_CLOCK_MEM, ps, &pMin, &pMax ) == nvml::NVML_SUCCESS )
      {
        NvmlPStateClockInfo info{};
        info.pstate = static_cast< unsigned int >( ps );
        info.minMHz = pMin;
        info.maxMHz = pMax;

        // Clock offset
        if ( m_getClockOffsets )
        {
          nvml::nvmlClockOffset_t offsetInfo{};
          offsetInfo.version = NVML_CLOCK_OFFSET_VER1;
          offsetInfo.type = nvml::NVML_CLOCK_MEM;
          offsetInfo.pstate = ps;
          if ( m_getClockOffsets( device, &offsetInfo ) == nvml::NVML_SUCCESS )
          {
            info.currentOffset = offsetInfo.clockOffsetMHz;
            info.minOffset = offsetInfo.minClockOffsetMHz;
            info.maxOffset = offsetInfo.maxClockOffsetMHz;
          }
        }

        int key = offsetKey( nvml::NVML_CLOCK_MEM, ps );
        if ( info.currentOffset == 0 )
        {
          auto devIt = m_appliedOffsets.find( deviceIndex );
          if ( devIt != m_appliedOffsets.end() )
          {
            auto offIt = devIt->second.find( key );
            if ( offIt != devIt->second.end() )
              info.currentOffset = offIt->second;
          }
        }

        {
          auto devIt = m_writableOffsets.find( deviceIndex );
          if ( devIt != m_writableOffsets.end() )
          {
            auto writableIt = devIt->second.find( key );
            if ( writableIt != devIt->second.end() )
              info.offsetWritable = writableIt->second;
          }
        }

        state.vramPStates.push_back( info );
        vramMinAll = std::min( vramMinAll, pMin );
        vramMaxAll = std::max( vramMaxAll, pMax );
      }
    }
  }

  if ( gpuMinAll != UINT32_MAX )
    state.gpuClockRange = { gpuMinAll, gpuMaxAll };
  if ( vramMinAll != UINT32_MAX )
    state.vramClockRange = { vramMinAll, vramMaxAll };

  // Check locked clocks support (presence of the functions is a good indicator)
  state.lockedClocksSupported = ( m_setGpuLockedClocks != nullptr );

  // Report applied locked clocks
  {
    auto it = m_appliedGpuLockedClocks.find( deviceIndex );
    if ( it != m_appliedGpuLockedClocks.end() )
      state.gpuLockedClocks = it->second;
  }
  {
    auto it = m_appliedVramLockedClocks.find( deviceIndex );
    if ( it != m_appliedVramLockedClocks.end() )
      state.vramLockedClocks = it->second;
  }

  return state;
}

bool NvmlWrapper::setClockOffset( unsigned int deviceIndex,
                                  nvml::nvmlClockType_t clockType,
                                  nvml::nvmlPstates_t pstate,
                                  int offsetMHz )
{
  if ( !m_setClockOffsets )
    return false;

  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt )
    return false;

  nvml::nvmlClockOffset_t info{};
  info.version = NVML_CLOCK_OFFSET_VER1;
  info.type = clockType;
  info.pstate = pstate;
  info.clockOffsetMHz = offsetMHz;

  nvml::nvmlReturn_t ret = m_setClockOffsets( *devOpt, &info );
  if ( ret == nvml::NVML_SUCCESS )
  {
    int key = offsetKey( clockType, pstate );
    m_appliedOffsets[deviceIndex][key] = offsetMHz;
    return true;
  }

  if ( isExpectedOcWriteRejection( ret ) )
    return offsetMHz == 0;

  std::cerr << "[NvmlWrapper] setClockOffset failed for type=" << clockType
            << " pstate=" << pstate << " offset=" << offsetMHz
            << " error=" << ret << std::endl;
  return false;
}

bool NvmlWrapper::isClockOffsetWritable( unsigned int deviceIndex,
                                         nvml::nvmlClockType_t clockType,
                                         nvml::nvmlPstates_t pstate ) const
{
  auto devIt = m_writableOffsets.find( deviceIndex );
  if ( devIt == m_writableOffsets.end() )
    return false;

  auto keyIt = devIt->second.find( offsetKey( clockType, pstate ) );
  if ( keyIt == devIt->second.end() )
    return false;

  return keyIt->second;
}

bool NvmlWrapper::setGpuLockedClocks( unsigned int deviceIndex,
                                      unsigned int minMHz,
                                      unsigned int maxMHz )
{
  if ( !m_setGpuLockedClocks )
    return false;

  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt )
    return false;

  nvml::nvmlReturn_t ret = m_setGpuLockedClocks( *devOpt, minMHz, maxMHz );
  if ( ret == nvml::NVML_SUCCESS )
  {
    m_appliedGpuLockedClocks[deviceIndex] = { minMHz, maxMHz };
    return true;
  }

  if ( isExpectedOcWriteRejection( ret ) )
    return false;

  std::cerr << "[NvmlWrapper] setGpuLockedClocks failed: " << ret << std::endl;
  return false;
}

bool NvmlWrapper::setVramLockedClocks( unsigned int deviceIndex,
                                       unsigned int minMHz,
                                       unsigned int maxMHz )
{
  if ( !m_setMemLockedClocks )
    return false;

  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt )
    return false;

  nvml::nvmlReturn_t ret = m_setMemLockedClocks( *devOpt, minMHz, maxMHz );
  if ( ret == nvml::NVML_SUCCESS )
  {
    m_appliedVramLockedClocks[deviceIndex] = { minMHz, maxMHz };
    return true;
  }

  if ( isExpectedOcWriteRejection( ret ) )
    return false;

  std::cerr << "[NvmlWrapper] setVramLockedClocks failed: " << ret << std::endl;
  return false;
}

bool NvmlWrapper::resetGpuLockedClocks( unsigned int deviceIndex )
{
  if ( !m_resetGpuLockedClocks )
    return false;

  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt )
    return false;

  nvml::nvmlReturn_t ret = m_resetGpuLockedClocks( *devOpt );
  if ( ret == nvml::NVML_SUCCESS )
  {
    m_appliedGpuLockedClocks.erase( deviceIndex );
    return true;
  }

  std::cerr << "[NvmlWrapper] resetGpuLockedClocks failed: " << ret << std::endl;
  return false;
}

bool NvmlWrapper::resetVramLockedClocks( unsigned int deviceIndex )
{
  if ( !m_resetMemLockedClocks )
    return false;

  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt )
    return false;

  nvml::nvmlReturn_t ret = m_resetMemLockedClocks( *devOpt );
  if ( ret == nvml::NVML_SUCCESS )
  {
    m_appliedVramLockedClocks.erase( deviceIndex );
    return true;
  }

  std::cerr << "[NvmlWrapper] resetVramLockedClocks failed: " << ret << std::endl;
  return false;
}

bool NvmlWrapper::resetAllClockOffsets( unsigned int deviceIndex )
{
  if ( !m_setClockOffsets || !m_getSupportedPstates )
    return false;

  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt )
    return false;

  auto device = *devOpt;
  nvml::nvmlPstates_t pstateArr[nvml::NVML_MAX_GPU_PERF_PSTATES];
  std::memset( pstateArr, 0xFF, sizeof( pstateArr ) );
  if ( m_getSupportedPstates( device, pstateArr, nvml::NVML_MAX_GPU_PERF_PSTATES ) != nvml::NVML_SUCCESS )
    return false;

  bool allOk = true;
  for ( unsigned int i = 0; i < nvml::NVML_MAX_GPU_PERF_PSTATES; ++i )
  {
    if ( pstateArr[i] == nvml::NVML_PSTATE_UNKNOWN )
      break;

    for ( auto clockType : { nvml::NVML_CLOCK_GRAPHICS, nvml::NVML_CLOCK_MEM } )
    {
      nvml::nvmlClockOffset_t info{};
      info.version = NVML_CLOCK_OFFSET_VER1;
      info.type = clockType;
      info.pstate = pstateArr[i];
      info.clockOffsetMHz = 0;

      if ( m_setClockOffsets( device, &info ) != nvml::NVML_SUCCESS )
        allOk = false;
    }
  }

  m_appliedOffsets.erase( deviceIndex );
  return allOk;
}

bool NvmlWrapper::setPowerLimit( unsigned int deviceIndex, unsigned int milliwatts )
{
  if ( !m_setPowerLimit )
    return false;

  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt )
    return false;

  nvml::nvmlReturn_t ret = m_setPowerLimit( *devOpt, milliwatts );
  if ( ret != nvml::NVML_SUCCESS )
  {
    std::cerr << "[NvmlWrapper] setPowerLimit failed: " << ret << std::endl;
    return false;
  }
  return true;
}

bool NvmlWrapper::resetPowerLimit( unsigned int deviceIndex )
{
  if ( !m_setPowerLimit || !m_getPowerLimitDefault )
    return false;

  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt )
    return false;

  unsigned int defaultMw = 0;
  if ( m_getPowerLimitDefault( *devOpt, &defaultMw ) != nvml::NVML_SUCCESS )
    return false;

  return m_setPowerLimit( *devOpt, defaultMw ) == nvml::NVML_SUCCESS;
}

// ---- Live monitoring getters (replace nvidia-smi subprocess calls) ----

std::optional< unsigned int > NvmlWrapper::getTemperatureDegC( unsigned int deviceIndex ) const noexcept
{
  if ( !m_getTemperature ) return std::nullopt;
  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt ) return std::nullopt;
  unsigned int temp = 0;
  // 0 = NVML_TEMPERATURE_GPU
  if ( m_getTemperature( *devOpt, 0, &temp ) != nvml::NVML_SUCCESS ) return std::nullopt;
  return temp;
}

std::optional< double > NvmlWrapper::getPowerDrawW( unsigned int deviceIndex ) const noexcept
{
  if ( !m_getPowerUsage ) return std::nullopt;
  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt ) return std::nullopt;
  unsigned int mw = 0;
  if ( m_getPowerUsage( *devOpt, &mw ) != nvml::NVML_SUCCESS ) return std::nullopt;
  return static_cast< double >( mw ) / 1000.0;
}

std::optional< double > NvmlWrapper::getEnforcedPowerLimitW( unsigned int deviceIndex ) const noexcept
{
  if ( !m_getEnforcedPowerLimit ) return std::nullopt;
  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt ) return std::nullopt;
  unsigned int mw = 0;
  if ( m_getEnforcedPowerLimit( *devOpt, &mw ) != nvml::NVML_SUCCESS ) return std::nullopt;
  return static_cast< double >( mw ) / 1000.0;
}

std::optional< double > NvmlWrapper::getPowerMaxLimitW( unsigned int deviceIndex ) const noexcept
{
  if ( !m_getPowerLimitConstraints ) return std::nullopt;
  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt ) return std::nullopt;
  unsigned int minMw = 0, maxMw = 0;
  if ( m_getPowerLimitConstraints( *devOpt, &minMw, &maxMw ) != nvml::NVML_SUCCESS ) return std::nullopt;
  return static_cast< double >( maxMw ) / 1000.0;
}

std::optional< double > NvmlWrapper::getPowerDefaultLimitW( unsigned int deviceIndex ) const noexcept
{
  if ( !m_getPowerLimitDefault ) return std::nullopt;
  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt ) return std::nullopt;
  unsigned int mw = 0;
  if ( m_getPowerLimitDefault( *devOpt, &mw ) != nvml::NVML_SUCCESS ) return std::nullopt;
  return static_cast< double >( mw ) / 1000.0;
}

std::optional< unsigned int > NvmlWrapper::getGpuClockMHz( unsigned int deviceIndex ) const noexcept
{
  if ( !m_getClockInfo ) return std::nullopt;
  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt ) return std::nullopt;
  unsigned int clk = 0;
  // NVML_CLOCK_GRAPHICS = 0
  if ( m_getClockInfo( *devOpt, 0, &clk ) != nvml::NVML_SUCCESS ) return std::nullopt;
  return clk;
}

std::optional< unsigned int > NvmlWrapper::getMaxGpuClockMHz( unsigned int deviceIndex ) const noexcept
{
  if ( !m_getMaxClockInfo ) return std::nullopt;
  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt ) return std::nullopt;
  unsigned int clk = 0;
  // NVML_CLOCK_GRAPHICS = 0
  if ( m_getMaxClockInfo( *devOpt, 0, &clk ) != nvml::NVML_SUCCESS ) return std::nullopt;
  return clk;
}

std::optional< unsigned int > NvmlWrapper::getMemClockMHz( unsigned int deviceIndex ) const noexcept
{
  if ( !m_getClockInfo ) return std::nullopt;
  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt ) return std::nullopt;
  unsigned int clk = 0;
  if ( m_getClockInfo( *devOpt, nvml::NVML_CLOCK_MEM, &clk ) != nvml::NVML_SUCCESS ) return std::nullopt;
  return clk;
}

std::optional< unsigned int > NvmlWrapper::getCoreVoltageMv( unsigned int deviceIndex ) const noexcept
{
  if ( !m_nvapiInitialized || !m_nvapiGetVoltage )
    return std::nullopt;
  if ( deviceIndex >= m_nvapiGpuHandles.size() )
    return std::nullopt;

  NvApiVoltage voltage{};
  voltage.version = ( static_cast< uint32_t >( sizeof( NvApiVoltage ) ) & 0xFFFFU ) | ( 1U << 16 );

  if ( m_nvapiGetVoltage( m_nvapiGpuHandles[deviceIndex], &voltage ) != static_cast< int32_t >( NVAPI_OK ) )
    return std::nullopt;
  if ( voltage.valueUv == 0 )
    return std::nullopt;

  return static_cast< unsigned int >( voltage.valueUv / 1000U );
}

std::optional< unsigned int > NvmlWrapper::getComputeUtilPct( unsigned int deviceIndex ) const noexcept
{
  if ( !m_getUtilizationRates ) return std::nullopt;
  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt ) return std::nullopt;
  nvml::nvmlUtilization_t util{};
  if ( m_getUtilizationRates( *devOpt, &util ) != nvml::NVML_SUCCESS ) return std::nullopt;
  return util.gpu;
}

std::optional< unsigned int > NvmlWrapper::getMemoryUtilPct( unsigned int deviceIndex ) const noexcept
{
  if ( !m_getUtilizationRates ) return std::nullopt;
  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt ) return std::nullopt;
  nvml::nvmlUtilization_t util{};
  if ( m_getUtilizationRates( *devOpt, &util ) != nvml::NVML_SUCCESS ) return std::nullopt;
  return util.memory;
}

std::optional< unsigned int > NvmlWrapper::getVramUsedMiB( unsigned int deviceIndex ) const noexcept
{
  if ( !m_getMemoryInfo ) return std::nullopt;
  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt ) return std::nullopt;
  nvml::nvmlMemory_t mem{};
  if ( m_getMemoryInfo( *devOpt, &mem ) != nvml::NVML_SUCCESS ) return std::nullopt;
  return static_cast< unsigned int >( mem.used / ( 1024ULL * 1024ULL ) );
}

std::optional< unsigned int > NvmlWrapper::getVramTotalMiB( unsigned int deviceIndex ) const noexcept
{
  if ( !m_getMemoryInfo ) return std::nullopt;
  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt ) return std::nullopt;
  nvml::nvmlMemory_t mem{};
  if ( m_getMemoryInfo( *devOpt, &mem ) != nvml::NVML_SUCCESS ) return std::nullopt;
  return static_cast< unsigned int >( mem.total / ( 1024ULL * 1024ULL ) );
}

std::optional< std::string > NvmlWrapper::getPerfLimitReason( unsigned int deviceIndex ) const noexcept
{
  if ( !m_getCurrentClocksThrottleReasons ) return std::nullopt;
  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt ) return std::nullopt;

  nvml::nvmlClocksThrottleReasons_t reasons = 0;
  if ( m_getCurrentClocksThrottleReasons( *devOpt, &reasons ) != nvml::NVML_SUCCESS )
    return std::nullopt;

  if ( reasons & nvml::NVML_CLOCKS_THROTTLE_REASON_HW_POWER_BRAKE_SLOWDOWN ) return std::string( "HW Power Brake" );
  if ( reasons & nvml::NVML_CLOCKS_THROTTLE_REASON_SW_POWER_CAP ) return std::string( "Power Limit" );
  if ( reasons & nvml::NVML_CLOCKS_THROTTLE_REASON_HW_THERMAL_SLOWDOWN ) return std::string( "HW Thermal" );
  if ( reasons & nvml::NVML_CLOCKS_THROTTLE_REASON_SW_THERMAL_SLOWDOWN ) return std::string( "SW Thermal" );
  if ( reasons & nvml::NVML_CLOCKS_THROTTLE_REASON_HW_SLOWDOWN ) return std::string( "HW Slowdown" );
  if ( reasons & nvml::NVML_CLOCKS_THROTTLE_REASON_APPLICATIONS_CLOCKS_SETTING ) return std::string( "App Clocks" );
  if ( reasons & nvml::NVML_CLOCKS_THROTTLE_REASON_DISPLAY_CLOCK_SETTING ) return std::string( "Display Limit" );
  if ( reasons & nvml::NVML_CLOCKS_THROTTLE_REASON_SYNC_BOOST ) return std::string( "Sync Boost" );
  if ( reasons & nvml::NVML_CLOCKS_THROTTLE_REASON_GPU_IDLE ) return std::string( "Idle" );
  return std::string( "None" );
}

std::optional< unsigned int > NvmlWrapper::getEncoderUtilPct( unsigned int deviceIndex ) const noexcept
{
  if ( !m_getEncoderUtilization ) return std::nullopt;
  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt ) return std::nullopt;
  unsigned int util = 0;
  unsigned int sampleUs = 0;
  if ( m_getEncoderUtilization( *devOpt, &util, &sampleUs ) != nvml::NVML_SUCCESS ) return std::nullopt;
  return util;
}

std::optional< unsigned int > NvmlWrapper::getDecoderUtilPct( unsigned int deviceIndex ) const noexcept
{
  if ( !m_getDecoderUtilization ) return std::nullopt;
  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt ) return std::nullopt;
  unsigned int util = 0;
  unsigned int sampleUs = 0;
  if ( m_getDecoderUtilization( *devOpt, &util, &sampleUs ) != nvml::NVML_SUCCESS ) return std::nullopt;
  return util;
}

std::optional< unsigned int > NvmlWrapper::getCurrentPstate( unsigned int deviceIndex ) const noexcept
{
  if ( !m_getPerformanceState ) return std::nullopt;
  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt ) return std::nullopt;
  nvml::nvmlPstates_t pstate = nvml::NVML_PSTATE_UNKNOWN;
  if ( m_getPerformanceState( *devOpt, &pstate ) != nvml::NVML_SUCCESS ) return std::nullopt;
  if ( pstate == nvml::NVML_PSTATE_UNKNOWN ) return std::nullopt;
  return static_cast< unsigned int >( pstate );
}

std::optional< int > NvmlWrapper::getGrClockOffsetMHz( unsigned int deviceIndex ) const noexcept
{
  if ( !m_getClockOffsets || !m_getPerformanceState ) return std::nullopt;
  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt ) return std::nullopt;
  nvml::nvmlPstates_t pstate = nvml::NVML_PSTATE_UNKNOWN;
  if ( m_getPerformanceState( *devOpt, &pstate ) != nvml::NVML_SUCCESS ) return std::nullopt;
  if ( pstate == nvml::NVML_PSTATE_UNKNOWN ) return std::nullopt;
  nvml::nvmlClockOffset_t info{};
  info.version = NVML_CLOCK_OFFSET_VER1;
  info.type    = nvml::NVML_CLOCK_GRAPHICS;
  info.pstate  = pstate;
  if ( m_getClockOffsets( *devOpt, &info ) != nvml::NVML_SUCCESS ) return std::nullopt;
  return info.clockOffsetMHz;
}

std::optional< int > NvmlWrapper::getMemClockOffsetMHz( unsigned int deviceIndex ) const noexcept
{
  if ( !m_getClockOffsets || !m_getPerformanceState ) return std::nullopt;
  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt ) return std::nullopt;
  nvml::nvmlPstates_t pstate = nvml::NVML_PSTATE_UNKNOWN;
  if ( m_getPerformanceState( *devOpt, &pstate ) != nvml::NVML_SUCCESS ) return std::nullopt;
  if ( pstate == nvml::NVML_PSTATE_UNKNOWN ) return std::nullopt;
  nvml::nvmlClockOffset_t info{};
  info.version = NVML_CLOCK_OFFSET_VER1;
  info.type    = nvml::NVML_CLOCK_MEM;
  info.pstate  = pstate;
  if ( m_getClockOffsets( *devOpt, &info ) != nvml::NVML_SUCCESS ) return std::nullopt;
  return info.clockOffsetMHz;
}
