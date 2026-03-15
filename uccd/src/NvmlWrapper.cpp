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
#include <chrono>
#include <cstring>

namespace
{
constexpr uint32_t NVAPI_INITIALIZE_ID = 0x0150E828;
constexpr uint32_t NVAPI_UNLOAD_ID = 0xD22BDD7E;
constexpr uint32_t NVAPI_ENUM_PHYSICAL_GPUS_ID = 0xE5AC921F;
constexpr uint32_t NVAPI_VOLTAGE_ID = 0x465F9BCF;
constexpr uint32_t NVAPI_ALL_CLOCK_FREQUENCIES_ID = 0xDCB616C3;
constexpr uint32_t NVAPI_PERF_POLICIES_STATUS_ID = 0x3D358A0C;
constexpr uint32_t NVAPI_CLIENT_POWER_TOPOLOGY_ID = 0x60DED2ED;
constexpr uint32_t NVAPI_OK = 0;
constexpr nvml::nvmlReturn_t NVML_ERROR_NO_PERMISSION = 4;
constexpr nvml::nvmlReturn_t NVML_ERROR_RESET_REQUIRED = 16;
constexpr unsigned int NVML_FI_DEV_POWER_MIN_LIMIT = 187;
constexpr unsigned int NVML_FI_DEV_POWER_MAX_LIMIT = 188;
constexpr unsigned int NVML_FI_DEV_POWER_DEFAULT_LIMIT = 189;
constexpr unsigned int NVML_FI_DEV_GET_GPU_RECOVERY_ACTION = 230;

inline bool isExpectedOcWriteRejection( nvml::nvmlReturn_t ret )
{
  return ret == nvml::NVML_ERROR_NOT_SUPPORTED || ret == NVML_ERROR_NO_PERMISSION;
}

inline bool isResetRequired( nvml::nvmlReturn_t ret )
{
  return ret == NVML_ERROR_RESET_REQUIRED;
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
  m_getPciInfo = loadSym< DeviceGetPciInfoFn >( "nvmlDeviceGetPciInfo_v3" );
  m_getTemperature = loadSym< DeviceGetTemperatureFn >( "nvmlDeviceGetTemperature" );
  m_getTemperatureThreshold = loadSym< DeviceGetTemperatureThresholdFn >( "nvmlDeviceGetTemperatureThreshold" );
  m_getMarginTemperature = loadSym< DeviceGetMarginTemperatureFn >( "nvmlDeviceGetMarginTemperature" );
  m_getPowerUsage = loadSym< DeviceGetPowerUsageFn >( "nvmlDeviceGetPowerUsage" );
  m_getPowerLimit = loadSym< DeviceGetPowerManagementLimitFn >( "nvmlDeviceGetPowerManagementLimit" );
  m_setPowerLimit = loadSym< DeviceSetPowerManagementLimitFn >( "nvmlDeviceSetPowerManagementLimit" );
  m_getPowerLimitConstraints = loadSym< DeviceGetPowerManagementLimitConstraintsFn >( "nvmlDeviceGetPowerManagementLimitConstraints" );
  m_getPowerLimitDefault = loadSym< DeviceGetPowerManagementDefaultLimitFn >( "nvmlDeviceGetPowerManagementDefaultLimit" );
  m_getFieldValues = loadSym< DeviceGetFieldValuesFn >( "nvmlDeviceGetFieldValues" );
  m_getPerformanceState = loadSym< DeviceGetPerformanceStateFn >( "nvmlDeviceGetPerformanceState" );
  m_getClockInfo = loadSym< DeviceGetClockInfoFn >( "nvmlDeviceGetClockInfo" );
  m_getMaxClockInfo = loadSym< DeviceGetMaxClockInfoFn >( "nvmlDeviceGetMaxClockInfo" );
  m_getEnforcedPowerLimit = loadSym< DeviceGetEnforcedPowerLimitFn >( "nvmlDeviceGetEnforcedPowerLimit" );
  m_getUtilizationRates   = loadSym< DeviceGetUtilizationRatesFn >( "nvmlDeviceGetUtilizationRates" );
  m_getMemoryInfoV2 = loadSym< DeviceGetMemoryInfoV2Fn >( "nvmlDeviceGetMemoryInfo_v2" );
  if ( !m_getMemoryInfoV2 )
    m_getMemoryInfo = loadSym< DeviceGetMemoryInfoFn >( "nvmlDeviceGetMemoryInfo" );
  m_getCurrentClocksThrottleReasons = loadSym< DeviceGetCurrentClocksThrottleReasonsFn >( "nvmlDeviceGetCurrentClocksThrottleReasons" );
  m_getCurrentClocksEventReasons = loadSym< DeviceGetCurrentClocksEventReasonsFn >( "nvmlDeviceGetCurrentClocksEventReasons" );
  m_getTotalEnergyConsumption = loadSym< DeviceGetTotalEnergyConsumptionFn >( "nvmlDeviceGetTotalEnergyConsumption" );
  m_getViolationStatus = loadSym< DeviceGetViolationStatusFn >( "nvmlDeviceGetViolationStatus" );
  m_getEncoderUtilization = loadSym< DeviceGetEncoderUtilizationFn >( "nvmlDeviceGetEncoderUtilization" );
  m_getDecoderUtilization = loadSym< DeviceGetDecoderUtilizationFn >( "nvmlDeviceGetDecoderUtilization" );
  m_getFanSpeedV2 = loadSym< DeviceGetFanSpeedV2Fn >( "nvmlDeviceGetFanSpeed_v2" );
  m_getNumFans = loadSym< DeviceGetNumFansFn >( "nvmlDeviceGetNumFans" );
  m_setFanSpeedV2 = loadSym< DeviceSetFanSpeedV2Fn >( "nvmlDeviceSetFanSpeed_v2" );
  m_getTargetFanSpeed = loadSym< DeviceGetTargetFanSpeedFn >( "nvmlDeviceGetTargetFanSpeed" );
  m_getMinMaxFanSpeed = loadSym< DeviceGetMinMaxFanSpeedFn >( "nvmlDeviceGetMinMaxFanSpeed" );
  m_getFanControlPolicyV2 = loadSym< DeviceGetFanControlPolicyV2Fn >( "nvmlDeviceGetFanControlPolicy_v2" );
  m_setFanControlPolicy = loadSym< DeviceSetFanControlPolicyFn >( "nvmlDeviceSetFanControlPolicy" );
  m_setDefaultFanSpeedV2 = loadSym< DeviceSetDefaultFanSpeedV2Fn >( "nvmlDeviceSetDefaultFanSpeed_v2" );

  // OC-specific functions (may not exist on older drivers)
  m_getSupportedPstates = loadSym< DeviceGetSupportedPstatesFn >( "nvmlDeviceGetSupportedPerformanceStates" );
  m_getMinMaxClock = loadSym< DeviceGetMinMaxClockFn >( "nvmlDeviceGetMinMaxClockOfPState" );
  m_getClockOffsets = loadSym< DeviceGetClockOffsetsFn >( "nvmlDeviceGetClockOffsets" );
  m_setClockOffsets = loadSym< DeviceSetClockOffsetsFn >( "nvmlDeviceSetClockOffsets" );
  m_getGpcClkVfOffset = loadSym< DeviceGetGpcClkVfOffsetFn >( "nvmlDeviceGetGpcClkVfOffset" );
  m_setGpcClkVfOffset = loadSym< DeviceSetGpcClkVfOffsetFn >( "nvmlDeviceSetGpcClkVfOffset" );
  m_getMemClkVfOffset = loadSym< DeviceGetMemClkVfOffsetFn >( "nvmlDeviceGetMemClkVfOffset" );
  m_setMemClkVfOffset = loadSym< DeviceSetMemClkVfOffsetFn >( "nvmlDeviceSetMemClkVfOffset" );
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

    auto ret = m_getSupportedPstates( *devOpt, pstateArr, nvml::NVML_MAX_GPU_PERF_PSTATES );
    if ( ret != nvml::NVML_SUCCESS )
    {
      if ( isResetRequired( ret ) )
        m_resetRequiredCache[deviceIndex] = true;

      std::cerr << "[NvmlWrapper] cacheSupportedPstates: failed for GPU " << deviceIndex
                << " (error " << ret << ")"
                << ( isResetRequired( ret ) ? " — GPU requires reset!" : "" )
                << std::endl;
      continue;
    }

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
  const bool hasClockOffsetsApi = ( m_getClockOffsets && m_setClockOffsets );
  const bool hasVfOffsetApi = ( m_getGpcClkVfOffset && m_setGpcClkVfOffset );

  if ( !hasClockOffsetsApi && !hasVfOffsetApi )
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
        int key = offsetKey( clockType, pstate );

        // Try versioned ClockOffsets API first
        if ( hasClockOffsetsApi )
        {
          nvml::nvmlClockOffset_t info{};
          info.version = NVML_CLOCK_OFFSET_VER1;
          info.type = clockType;
          info.pstate = pstate;

          if ( m_getClockOffsets( device, &info ) != nvml::NVML_SUCCESS )
          {
            m_writableOffsets[deviceIndex][key] = false;
            continue;
          }

          nvml::nvmlClockOffset_t writeInfo{};
          writeInfo.version = NVML_CLOCK_OFFSET_VER1;
          writeInfo.type = clockType;
          writeInfo.pstate = pstate;
          writeInfo.clockOffsetMHz = info.clockOffsetMHz;

          const auto ret = m_setClockOffsets( device, &writeInfo );
          m_writableOffsets[deviceIndex][key] = ( ret == nvml::NVML_SUCCESS );
          continue;
        }

        // Fallback: VfOffset APIs (no per-pstate support, only P0 is meaningful)
        if ( pstate != nvml::NVML_PSTATE_0 )
        {
          m_writableOffsets[deviceIndex][key] = false;
          continue;
        }

        bool readable = false;
        if ( clockType == nvml::NVML_CLOCK_GRAPHICS && m_getGpcClkVfOffset )
        {
          int off = 0;
          readable = ( m_getGpcClkVfOffset( device, &off ) == nvml::NVML_SUCCESS );
        }
        else if ( clockType == nvml::NVML_CLOCK_MEM && m_getMemClkVfOffset )
        {
          int off = 0;
          readable = ( m_getMemClkVfOffset( device, &off ) == nvml::NVML_SUCCESS );
        }

        m_writableOffsets[deviceIndex][key] = readable;
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
  m_nvapiGetAllClockFrequencies = reinterpret_cast< NvApiGetAllClockFrequenciesFn >( m_nvapiQueryInterface( NVAPI_ALL_CLOCK_FREQUENCIES_ID ) );
  m_nvapiPerfPoliciesGetStatus = reinterpret_cast< NvApiPerfPoliciesGetStatusFn >( m_nvapiQueryInterface( NVAPI_PERF_POLICIES_STATUS_ID ) );
  m_nvapiClientPowerTopologyGetInfo = reinterpret_cast< NvApiClientPowerTopologyGetInfoFn >( m_nvapiQueryInterface( NVAPI_CLIENT_POWER_TOPOLOGY_ID ) );

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

  // PCI bus id (domain:bus:device.function)
  if ( m_getPciInfo )
  {
    nvml::nvmlPciInfo_t pciInfo{};
    if ( m_getPciInfo( device, &pciInfo ) == nvml::NVML_SUCCESS )
      state.pciBusId = pciInfo.busId;
  }

  // Check for reset-required state early
  state.resetRequired = needsReset( deviceIndex );

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
    if ( m_getTemperatureThreshold( device, 1 /* Slowdown */, &threshold ) == nvml::NVML_SUCCESS )
      state.tempSlowdownC = threshold;
    if ( m_getTemperatureThreshold( device, 3 /* GpuMax */, &threshold ) == nvml::NVML_SUCCESS )
      state.tempGpuMaxC = threshold;
  }
  if ( m_getMarginTemperature )
  {
    unsigned int margin = 0;
    if ( m_getMarginTemperature( device, &margin ) == nvml::NVML_SUCCESS )
      state.thermalMarginC = static_cast< int >( margin );
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

        // Clock offset — try versioned ClockOffsets API, then VfOffset fallback
        if ( m_getClockOffsets )
        {
          nvml::nvmlClockOffset_t offsetInfo{};
          offsetInfo.version = NVML_CLOCK_OFFSET_VER1;
          offsetInfo.type = nvml::NVML_CLOCK_GRAPHICS;
          offsetInfo.pstate = ps;
          if ( m_getClockOffsets( device, &offsetInfo ) == nvml::NVML_SUCCESS )
          {
            info.currentOffset = offsetInfo.clockOffsetMHz;
            info.minOffset = std::max( offsetInfo.minClockOffsetMHz, NvmlOffsetCaps::GPU_MIN_OFFSET );
            info.maxOffset = std::min( offsetInfo.maxClockOffsetMHz, NvmlOffsetCaps::GPU_MAX_OFFSET );
            state.offsetsSupported = true;
          }
        }
        else if ( m_getGpcClkVfOffset && ps == nvml::NVML_PSTATE_0 )
        {
          int off = 0;
          if ( m_getGpcClkVfOffset( device, &off ) == nvml::NVML_SUCCESS )
          {
            info.currentOffset = off;
            info.minOffset = NvmlOffsetCaps::GPU_MIN_OFFSET;
            info.maxOffset = NvmlOffsetCaps::GPU_MAX_OFFSET;
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

        // Clock offset — try versioned ClockOffsets API, then VfOffset fallback
        if ( m_getClockOffsets )
        {
          nvml::nvmlClockOffset_t offsetInfo{};
          offsetInfo.version = NVML_CLOCK_OFFSET_VER1;
          offsetInfo.type = nvml::NVML_CLOCK_MEM;
          offsetInfo.pstate = ps;
          if ( m_getClockOffsets( device, &offsetInfo ) == nvml::NVML_SUCCESS )
          {
            info.currentOffset = offsetInfo.clockOffsetMHz;
            info.minOffset = std::max( offsetInfo.minClockOffsetMHz, NvmlOffsetCaps::VRAM_MIN_OFFSET );
            info.maxOffset = std::min( offsetInfo.maxClockOffsetMHz, NvmlOffsetCaps::VRAM_MAX_OFFSET );
          }
        }
        else if ( m_getMemClkVfOffset && ps == nvml::NVML_PSTATE_0 )
        {
          int off = 0;
          if ( m_getMemClkVfOffset( device, &off ) == nvml::NVML_SUCCESS )
          {
            info.currentOffset = off;
            info.minOffset = NvmlOffsetCaps::VRAM_MIN_OFFSET;
            info.maxOffset = NvmlOffsetCaps::VRAM_MAX_OFFSET;
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
  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt )
    return false;

  // Try versioned ClockOffsets API first
  if ( m_setClockOffsets )
  {
    nvml::nvmlClockOffset_t info{};
    info.version = NVML_CLOCK_OFFSET_VER1;
    info.type = clockType;
    info.pstate = pstate;
    info.clockOffsetMHz = offsetMHz;

    nvml::nvmlReturn_t ret = m_setClockOffsets( *devOpt, &info );
    if ( ret == nvml::NVML_SUCCESS )
    {
      if ( !m_getClockOffsets )
      {
        int key = offsetKey( clockType, pstate );
        m_appliedOffsets[deviceIndex][key] = offsetMHz;
        return true;
      }

      bool readbackMatches = false;
      {
        nvml::nvmlClockOffset_t verifyInfo{};
        verifyInfo.version = NVML_CLOCK_OFFSET_VER1;
        verifyInfo.type = clockType;
        verifyInfo.pstate = pstate;

        if ( m_getClockOffsets( *devOpt, &verifyInfo ) == nvml::NVML_SUCCESS )
          readbackMatches = ( verifyInfo.clockOffsetMHz == offsetMHz );
      }

      if ( readbackMatches )
      {
        int key = offsetKey( clockType, pstate );
        m_appliedOffsets[deviceIndex][key] = offsetMHz;
        return true;
      }

      // Some drivers accept the versioned per-P-state ClockOffsets call but
      // do not actually apply it. For P0, fall back to the global VfOffset
      // APIs when available. For other P-states, treat this as unsupported so
      // the UI stops presenting them as writable after a refresh.
      if ( pstate != nvml::NVML_PSTATE_0 )
      {
        const int key = offsetKey( clockType, pstate );
        m_writableOffsets[deviceIndex][key] = false;
        std::cerr << "[NvmlWrapper] setClockOffset: per-P-state write accepted but readback stayed at 0"
                  << " for type=" << clockType << " pstate=" << pstate
                  << "; marking as not writable" << std::endl;
        return offsetMHz == 0;
      }
    }

    if ( isResetRequired( ret ) )
    {
      std::cerr << "[NvmlWrapper] setClockOffset: GPU requires reset (error " << ret
                << ") for type=" << clockType << " pstate=" << pstate
                << " offset=" << offsetMHz << std::endl;
      m_resetRequiredCache[deviceIndex] = true;
      return false;
    }

    if ( isExpectedOcWriteRejection( ret ) )
      return offsetMHz == 0;

    std::cerr << "[NvmlWrapper] setClockOffset (ClockOffsets API) failed for type=" << clockType
              << " pstate=" << pstate << " offset=" << offsetMHz
              << " error=" << ret << std::endl;
    return false;
  }

  // Fallback: VfOffset APIs (global, not per-pstate — only P0 meaningful)
  nvml::nvmlReturn_t ret = nvml::NVML_ERROR_NOT_SUPPORTED;
  if ( clockType == nvml::NVML_CLOCK_GRAPHICS && m_setGpcClkVfOffset )
    ret = m_setGpcClkVfOffset( *devOpt, offsetMHz );
  else if ( clockType == nvml::NVML_CLOCK_MEM && m_setMemClkVfOffset )
    ret = m_setMemClkVfOffset( *devOpt, offsetMHz );

  if ( ret == nvml::NVML_SUCCESS )
  {
    int key = offsetKey( clockType, pstate );
    m_appliedOffsets[deviceIndex][key] = offsetMHz;
    return true;
  }

  if ( isResetRequired( ret ) )
  {
    std::cerr << "[NvmlWrapper] setClockOffset: GPU requires reset (VfOffset fallback)" << std::endl;
    m_resetRequiredCache[deviceIndex] = true;
    return false;
  }

  if ( isExpectedOcWriteRejection( ret ) )
    return offsetMHz == 0;

  std::cerr << "[NvmlWrapper] setClockOffset (VfOffset fallback) failed for type=" << clockType
            << " offset=" << offsetMHz << " error=" << ret << std::endl;
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

  if ( isResetRequired( ret ) )
  {
    std::cerr << "[NvmlWrapper] setGpuLockedClocks: GPU requires reset (error " << ret
              << "), attempting to undo locked clocks" << std::endl;
    m_resetRequiredCache[deviceIndex] = true;
    if ( m_resetGpuLockedClocks )
      m_resetGpuLockedClocks( *devOpt );
    return false;
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

  if ( isResetRequired( ret ) )
  {
    std::cerr << "[NvmlWrapper] setVramLockedClocks: GPU requires reset (error " << ret
              << "), attempting to undo all locked clocks" << std::endl;
    m_resetRequiredCache[deviceIndex] = true;
    // Try to undo both GPU and VRAM locked clocks to recover
    if ( m_resetMemLockedClocks )
      m_resetMemLockedClocks( *devOpt );
    if ( m_resetGpuLockedClocks )
      m_resetGpuLockedClocks( *devOpt );
    m_appliedGpuLockedClocks.erase( deviceIndex );
    m_appliedVramLockedClocks.erase( deviceIndex );
    return false;
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
  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt )
    return false;

  auto device = *devOpt;
  bool allOk = true;

  if ( m_setClockOffsets && m_getSupportedPstates )
  {
    nvml::nvmlPstates_t pstateArr[nvml::NVML_MAX_GPU_PERF_PSTATES];
    std::memset( pstateArr, 0xFF, sizeof( pstateArr ) );
    if ( m_getSupportedPstates( device, pstateArr, nvml::NVML_MAX_GPU_PERF_PSTATES ) == nvml::NVML_SUCCESS )
    {
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
  }

  // Fallback: VfOffset APIs (global reset)
  if ( m_setGpcClkVfOffset )
  {
    if ( m_setGpcClkVfOffset( device, 0 ) != nvml::NVML_SUCCESS )
      allOk = false;
  }
  if ( m_setMemClkVfOffset )
  {
    if ( m_setMemClkVfOffset( device, 0 ) != nvml::NVML_SUCCESS )
      allOk = false;
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
    if ( isResetRequired( ret ) )
      m_resetRequiredCache[deviceIndex] = true;
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

std::optional< std::string > NvmlWrapper::getDeviceName( unsigned int deviceIndex ) const
{
  if ( !m_getName ) return std::nullopt;
  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt ) return std::nullopt;

  char name[96] = {};
  if ( m_getName( *devOpt, name, sizeof( name ) ) != nvml::NVML_SUCCESS )
    return std::nullopt;

  return std::string( name );
}

std::optional< unsigned int > NvmlWrapper::getMarginTemperature( unsigned int deviceIndex ) const noexcept
{
  if ( !m_getMarginTemperature ) return std::nullopt;
  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt ) return std::nullopt;
  unsigned int margin = 0;
  if ( m_getMarginTemperature( *devOpt, &margin ) != nvml::NVML_SUCCESS ) return std::nullopt;
  return margin;
}

std::optional< double > NvmlWrapper::getPowerDrawW( unsigned int deviceIndex ) const noexcept
{
  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt ) return std::nullopt;

  // Primary: nvmlDeviceGetPowerUsage (milliwatts)
  if ( m_getPowerUsage )
  {
    unsigned int mw = 0;
    if ( m_getPowerUsage( *devOpt, &mw ) == nvml::NVML_SUCCESS )
      return static_cast< double >( mw ) / 1000.0;
  }

  // Fallback: compute power from energy consumption delta
  if ( m_getTotalEnergyConsumption )
  {
    unsigned long long energyMj = 0;
    if ( m_getTotalEnergyConsumption( *devOpt, &energyMj ) == nvml::NVML_SUCCESS )
    {
      auto now = std::chrono::steady_clock::now();
      auto prevIt = m_lastEnergyMj.find( deviceIndex );
      auto prevTimeIt = m_lastEnergyTime.find( deviceIndex );

      if ( prevIt != m_lastEnergyMj.end() && prevTimeIt != m_lastEnergyTime.end() )
      {
        auto dtMs = std::chrono::duration_cast< std::chrono::milliseconds >( now - prevTimeIt->second ).count();
        if ( dtMs > 0 )
        {
          double powerW = static_cast< double >( energyMj - prevIt->second ) / static_cast< double >( dtMs );
          m_lastComputedPowerW[deviceIndex] = powerW;
        }
      }

      m_lastEnergyMj[deviceIndex] = energyMj;
      m_lastEnergyTime[deviceIndex] = now;

      auto powerIt = m_lastComputedPowerW.find( deviceIndex );
      if ( powerIt != m_lastComputedPowerW.end() )
        return powerIt->second;
    }
  }

  return std::nullopt;
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
  if ( m_getPowerLimitConstraints )
  {
    auto devOpt = getDevice( deviceIndex );
    if ( devOpt )
    {
      unsigned int minMw = 0, maxMw = 0;
      if ( m_getPowerLimitConstraints( *devOpt, &minMw, &maxMw ) == nvml::NVML_SUCCESS )
        return static_cast< double >( maxMw ) / 1000.0;
    }
  }

  if ( const auto maxMw = getFieldUnsignedLong( deviceIndex, NVML_FI_DEV_POWER_MAX_LIMIT ) )
    return static_cast< double >( *maxMw ) / 1000.0;

  return std::nullopt;
}

std::optional< double > NvmlWrapper::getPowerMinLimitW( unsigned int deviceIndex ) const noexcept
{
  if ( m_getPowerLimitConstraints )
  {
    auto devOpt = getDevice( deviceIndex );
    if ( devOpt )
    {
      unsigned int minMw = 0, maxMw = 0;
      if ( m_getPowerLimitConstraints( *devOpt, &minMw, &maxMw ) == nvml::NVML_SUCCESS )
        return static_cast< double >( minMw ) / 1000.0;
    }
  }

  if ( const auto minMw = getFieldUnsignedLong( deviceIndex, NVML_FI_DEV_POWER_MIN_LIMIT ) )
    return static_cast< double >( *minMw ) / 1000.0;

  return std::nullopt;
}

std::optional< double > NvmlWrapper::getPowerDefaultLimitW( unsigned int deviceIndex ) const noexcept
{
  if ( m_getPowerLimitDefault )
  {
    auto devOpt = getDevice( deviceIndex );
    if ( devOpt )
    {
      unsigned int mw = 0;
      if ( m_getPowerLimitDefault( *devOpt, &mw ) == nvml::NVML_SUCCESS )
        return static_cast< double >( mw ) / 1000.0;
    }
  }

  if ( const auto defaultMw = getFieldUnsignedLong( deviceIndex, NVML_FI_DEV_POWER_DEFAULT_LIMIT ) )
    return static_cast< double >( *defaultMw ) / 1000.0;

  return std::nullopt;
}

std::optional< unsigned int > NvmlWrapper::getGpuRecoveryAction( unsigned int deviceIndex ) const noexcept
{
  return getFieldUnsignedInt( deviceIndex, NVML_FI_DEV_GET_GPU_RECOVERY_ACTION );
}

bool NvmlWrapper::needsReset( unsigned int deviceIndex ) const noexcept
{
  if ( !m_initialized )
    return false;

  // Check cached reset state from init-time probing
  auto cacheIt = m_resetRequiredCache.find( deviceIndex );
  if ( cacheIt != m_resetRequiredCache.end() && cacheIt->second )
    return true;

  // Also probe dynamically — try multiple NVML functions since some
  // return NOT_SUPPORTED (3) instead of RESET_REQUIRED (16) on certain GPUs
  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt )
    return false;

  if ( m_getClockInfo )
  {
    unsigned int clk = 0;
    if ( isResetRequired( m_getClockInfo( *devOpt, 0, &clk ) ) )
    {
      m_resetRequiredCache[deviceIndex] = true;
      return true;
    }
  }

  if ( m_getMinMaxClock )
  {
    unsigned int pMin = 0, pMax = 0;
    if ( isResetRequired( m_getMinMaxClock( *devOpt, nvml::NVML_CLOCK_GRAPHICS,
                                             nvml::NVML_PSTATE_0, &pMin, &pMax ) ) )
    {
      m_resetRequiredCache[deviceIndex] = true;
      return true;
    }
  }

  if ( m_getFanSpeedV2 )
  {
    unsigned int speed = 0;
    if ( isResetRequired( m_getFanSpeedV2( *devOpt, 0, &speed ) ) )
    {
      m_resetRequiredCache[deviceIndex] = true;
      return true;
    }
  }

  return false;
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
  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt ) return std::nullopt;

  // Use total−free (includes driver-reserved memory) to match nvidia-smi reporting.
  if ( m_getMemoryInfoV2 )
  {
    nvml::nvmlMemory_v2_t mem2{};
    mem2.version = NVML_MEMORY_V2_VER;
    if ( m_getMemoryInfoV2( *devOpt, &mem2 ) == nvml::NVML_SUCCESS )
      return static_cast< unsigned int >( ( mem2.total - mem2.free ) / ( 1024ULL * 1024ULL ) );
  }

  if ( !m_getMemoryInfo ) return std::nullopt;
  nvml::nvmlMemory_t mem{};
  if ( m_getMemoryInfo( *devOpt, &mem ) != nvml::NVML_SUCCESS ) return std::nullopt;
  return static_cast< unsigned int >( mem.used / ( 1024ULL * 1024ULL ) );
}

std::optional< unsigned int > NvmlWrapper::getVramTotalMiB( unsigned int deviceIndex ) const noexcept
{
  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt ) return std::nullopt;

  if ( m_getMemoryInfoV2 )
  {
    nvml::nvmlMemory_v2_t mem2{};
    mem2.version = NVML_MEMORY_V2_VER;
    if ( m_getMemoryInfoV2( *devOpt, &mem2 ) == nvml::NVML_SUCCESS )
      return static_cast< unsigned int >( mem2.total / ( 1024ULL * 1024ULL ) );
  }

  if ( !m_getMemoryInfo ) return std::nullopt;
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

std::optional< unsigned long long >
NvmlWrapper::getCurrentClocksThrottleReasonsMask( unsigned int deviceIndex ) const noexcept
{
  if ( !m_getCurrentClocksThrottleReasons ) return std::nullopt;
  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt ) return std::nullopt;

  nvml::nvmlClocksThrottleReasons_t reasons = 0;
  if ( m_getCurrentClocksThrottleReasons( *devOpt, &reasons ) != nvml::NVML_SUCCESS )
    return std::nullopt;
  return static_cast< unsigned long long >( reasons );
}

std::optional< unsigned long long >
NvmlWrapper::getCurrentClocksEventReasonsMask( unsigned int deviceIndex ) const noexcept
{
  if ( !m_getCurrentClocksEventReasons ) return std::nullopt;
  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt ) return std::nullopt;

  nvml::nvmlClocksEventReasons_t reasons = 0;
  if ( m_getCurrentClocksEventReasons( *devOpt, &reasons ) != nvml::NVML_SUCCESS )
    return std::nullopt;
  return static_cast< unsigned long long >( reasons );
}

std::optional< unsigned long long >
NvmlWrapper::getTotalEnergyConsumptionmJ( unsigned int deviceIndex ) const noexcept
{
  if ( !m_getTotalEnergyConsumption ) return std::nullopt;
  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt ) return std::nullopt;

  unsigned long long mj = 0;
  if ( m_getTotalEnergyConsumption( *devOpt, &mj ) != nvml::NVML_SUCCESS )
    return std::nullopt;
  return mj;
}

std::optional< unsigned long long >
NvmlWrapper::getPerfPolicyViolationUsec( unsigned int deviceIndex,
                                         nvml::nvmlPerfPolicyType_t policy ) const noexcept
{
  if ( !m_getViolationStatus ) return std::nullopt;
  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt ) return std::nullopt;

  nvml::nvmlViolationTime_t vt{};
  if ( m_getViolationStatus( *devOpt, policy, &vt ) != nvml::NVML_SUCCESS )
    return std::nullopt;
  return vt.violationTime;
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

std::optional< unsigned int > NvmlWrapper::getFanSpeedPct( unsigned int deviceIndex, unsigned int fanIndex ) const noexcept
{
  if ( !m_getFanSpeedV2 ) return std::nullopt;
  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt ) return std::nullopt;
  unsigned int speed = 0;
  if ( m_getFanSpeedV2( *devOpt, fanIndex, &speed ) != nvml::NVML_SUCCESS ) return std::nullopt;
  return speed;
}

std::optional< unsigned int > NvmlWrapper::getNumFans( unsigned int deviceIndex ) const noexcept
{
  if ( !m_getNumFans ) return std::nullopt;
  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt ) return std::nullopt;
  unsigned int count = 0;
  if ( m_getNumFans( *devOpt, &count ) != nvml::NVML_SUCCESS ) return std::nullopt;
  return count;
}

std::optional< unsigned int > NvmlWrapper::getTargetFanSpeedPct( unsigned int deviceIndex, unsigned int fanIndex ) const noexcept
{
  if ( !m_getTargetFanSpeed ) return std::nullopt;
  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt ) return std::nullopt;
  unsigned int target = 0;
  if ( m_getTargetFanSpeed( *devOpt, fanIndex, &target ) != nvml::NVML_SUCCESS ) return std::nullopt;
  return target;
}

std::optional< std::pair< unsigned int, unsigned int > > NvmlWrapper::getMinMaxFanSpeedPct( unsigned int deviceIndex ) const noexcept
{
  if ( !m_getMinMaxFanSpeed ) return std::nullopt;
  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt ) return std::nullopt;
  unsigned int minPct = 0, maxPct = 0;
  if ( m_getMinMaxFanSpeed( *devOpt, &minPct, &maxPct ) != nvml::NVML_SUCCESS ) return std::nullopt;
  return std::make_pair( minPct, maxPct );
}

std::optional< unsigned int > NvmlWrapper::getFanControlPolicy( unsigned int deviceIndex, unsigned int fanIndex ) const noexcept
{
  if ( !m_getFanControlPolicyV2 ) return std::nullopt;
  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt ) return std::nullopt;
  unsigned int policy = 0;
  if ( m_getFanControlPolicyV2( *devOpt, fanIndex, &policy ) != nvml::NVML_SUCCESS ) return std::nullopt;
  return policy;
}

bool NvmlWrapper::setFanSpeed( unsigned int deviceIndex, unsigned int fanIndex, unsigned int speedPct )
{
  if ( !m_setFanSpeedV2 ) return false;
  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt ) return false;
  return m_setFanSpeedV2( *devOpt, fanIndex, speedPct ) == nvml::NVML_SUCCESS;
}

bool NvmlWrapper::setFanControlPolicy( unsigned int deviceIndex, unsigned int fanIndex, unsigned int policy )
{
  if ( !m_setFanControlPolicy ) return false;
  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt ) return false;
  return m_setFanControlPolicy( *devOpt, fanIndex, policy ) == nvml::NVML_SUCCESS;
}

bool NvmlWrapper::resetFanSpeed( unsigned int deviceIndex, unsigned int fanIndex )
{
  if ( !m_setDefaultFanSpeedV2 ) return false;
  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt ) return false;
  return m_setDefaultFanSpeedV2( *devOpt, fanIndex ) == nvml::NVML_SUCCESS;
}

std::optional< int > NvmlWrapper::getGrClockOffsetMHz( unsigned int deviceIndex ) const noexcept
{
  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt ) return std::nullopt;

  // Try versioned ClockOffsets API
  if ( m_getClockOffsets && m_getPerformanceState )
  {
    nvml::nvmlPstates_t pstate = nvml::NVML_PSTATE_UNKNOWN;
    if ( m_getPerformanceState( *devOpt, &pstate ) == nvml::NVML_SUCCESS
         && pstate != nvml::NVML_PSTATE_UNKNOWN )
    {
      nvml::nvmlClockOffset_t info{};
      info.version = NVML_CLOCK_OFFSET_VER1;
      info.type    = nvml::NVML_CLOCK_GRAPHICS;
      info.pstate  = pstate;
      if ( m_getClockOffsets( *devOpt, &info ) == nvml::NVML_SUCCESS )
        return info.clockOffsetMHz;
    }
  }

  // Fallback: VfOffset API
  if ( m_getGpcClkVfOffset )
  {
    int off = 0;
    if ( m_getGpcClkVfOffset( *devOpt, &off ) == nvml::NVML_SUCCESS )
      return off;
  }

  return std::nullopt;
}

std::optional< int > NvmlWrapper::getMemClockOffsetMHz( unsigned int deviceIndex ) const noexcept
{
  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt ) return std::nullopt;

  // Try versioned ClockOffsets API
  if ( m_getClockOffsets && m_getPerformanceState )
  {
    nvml::nvmlPstates_t pstate = nvml::NVML_PSTATE_UNKNOWN;
    if ( m_getPerformanceState( *devOpt, &pstate ) == nvml::NVML_SUCCESS
         && pstate != nvml::NVML_PSTATE_UNKNOWN )
    {
      nvml::nvmlClockOffset_t info{};
      info.version = NVML_CLOCK_OFFSET_VER1;
      info.type    = nvml::NVML_CLOCK_MEM;
      info.pstate  = pstate;
      if ( m_getClockOffsets( *devOpt, &info ) == nvml::NVML_SUCCESS )
        return info.clockOffsetMHz;
    }
  }

  // Fallback: VfOffset API
  if ( m_getMemClkVfOffset )
  {
    int off = 0;
    if ( m_getMemClkVfOffset( *devOpt, &off ) == nvml::NVML_SUCCESS )
      return off;
  }

  return std::nullopt;
}

std::optional< unsigned int >
NvmlWrapper::getNvapiCurrentGraphicsClockMHz( unsigned int deviceIndex ) const noexcept
{
  // Try NvAPI first
  if ( m_nvapiInitialized && m_nvapiGetAllClockFrequencies
       && deviceIndex < m_nvapiGpuHandles.size() )
  {
    NvApiClockFrequenciesV3 data{};
    data.version = ( static_cast< uint32_t >( sizeof( NvApiClockFrequenciesV3 ) ) & 0xFFFFU ) | ( 3U << 16 );
    data.clockType = 0; // CURRENT

    if ( m_nvapiGetAllClockFrequencies( m_nvapiGpuHandles[deviceIndex], &data ) == static_cast< int32_t >( NVAPI_OK ) )
    {
      if ( data.domain[0].present != 0U && data.domain[0].freqKHz != 0U )
        return data.domain[0].freqKHz / 1000U;
    }
  }

  // Fallback: NVML getClockInfo(GRAPHICS)
  return getGpuClockMHz( deviceIndex );
}

std::optional< unsigned int >
NvmlWrapper::getNvapiCurrentSmClockMHz( unsigned int deviceIndex ) const noexcept
{
  // Try NvAPI first
  if ( m_nvapiInitialized && m_nvapiGetAllClockFrequencies
       && deviceIndex < m_nvapiGpuHandles.size() )
  {
    NvApiClockFrequenciesV3 data{};
    data.version = ( static_cast< uint32_t >( sizeof( NvApiClockFrequenciesV3 ) ) & 0xFFFFU ) | ( 3U << 16 );
    data.clockType = 0; // CURRENT

    if ( m_nvapiGetAllClockFrequencies( m_nvapiGpuHandles[deviceIndex], &data ) == static_cast< int32_t >( NVAPI_OK ) )
    {
      if ( data.domain[2].present != 0U && data.domain[2].freqKHz != 0U )
        return data.domain[2].freqKHz / 1000U;
    }
  }

  // Fallback: NVML getClockInfo(SM)
  if ( !m_getClockInfo ) return std::nullopt;
  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt ) return std::nullopt;
  unsigned int clk = 0;
  if ( m_getClockInfo( *devOpt, nvml::NVML_CLOCK_SM, &clk ) != nvml::NVML_SUCCESS ) return std::nullopt;
  return clk;
}

std::optional< unsigned int >
NvmlWrapper::getNvapiPerfLimiterMask( unsigned int deviceIndex ) const noexcept
{
  // Try NvAPI first
  if ( m_nvapiInitialized && m_nvapiPerfPoliciesGetStatus
       && deviceIndex < m_nvapiGpuHandles.size() )
  {
    NvApiPerfPoliciesStatus data{};
    data.version = ( static_cast< uint32_t >( sizeof( NvApiPerfPoliciesStatus ) ) & 0xFFFFU ) | ( 1U << 16 );

    if ( m_nvapiPerfPoliciesGetStatus( m_nvapiGpuHandles[deviceIndex], &data ) == static_cast< int32_t >( NVAPI_OK ) )
    {
      const uint32_t *u32 = reinterpret_cast< const uint32_t * >( &data );
      return u32[6];
    }
  }

  // Fallback: derive a synthetic limiter mask from NVML throttle reasons.
  // Bit 0 = power limiting, bit 1 = thermal limiting.
  auto maskOpt = getCurrentClocksThrottleReasonsMask( deviceIndex );
  if ( !maskOpt )
    return std::nullopt;

  const auto mask = static_cast< nvml::nvmlClocksThrottleReasons_t >( *maskOpt );
  unsigned int synth = 0;
  if ( mask & nvml::NVML_CLOCKS_THROTTLE_REASON_SW_POWER_CAP )
    synth |= 1U;
  if ( mask & ( nvml::NVML_CLOCKS_THROTTLE_REASON_HW_THERMAL_SLOWDOWN
              | nvml::NVML_CLOCKS_THROTTLE_REASON_SW_THERMAL_SLOWDOWN
              | nvml::NVML_CLOCKS_THROTTLE_REASON_HW_SLOWDOWN ) )
    synth |= 2U;
  return synth;
}

std::optional< unsigned int >
NvmlWrapper::getNvapiClientPowerBudgetW( unsigned int deviceIndex ) const noexcept
{
  // Try NvAPI first
  if ( m_nvapiInitialized && m_nvapiClientPowerTopologyGetInfo
       && deviceIndex < m_nvapiGpuHandles.size() )
  {
    NvApiClientPowerTopology data{};
    data.version = ( static_cast< uint32_t >( sizeof( NvApiClientPowerTopology ) ) & 0xFFFFU ) | ( 1U << 16 );

    if ( m_nvapiClientPowerTopologyGetInfo( m_nvapiGpuHandles[deviceIndex], &data ) == static_cast< int32_t >( NVAPI_OK ) )
    {
      const uint32_t *u32 = reinterpret_cast< const uint32_t * >( &data );
      const uint32_t budgetW = u32[9];
      if ( budgetW != 0U )
        return budgetW;
    }
  }

  // Fallback: NVML enforced power limit (in milliwatts → watts)
  auto limitOptW = getEnforcedPowerLimitW( deviceIndex );
  if ( limitOptW && *limitOptW > 0.0 )
    return static_cast< unsigned int >( *limitOptW );

  return std::nullopt;
}

std::optional< unsigned long >
NvmlWrapper::getFieldUnsignedLong( unsigned int deviceIndex,
                                   unsigned int fieldId,
                                   unsigned int scopeId ) const noexcept
{
  if ( !m_getFieldValues )
    return std::nullopt;

  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt )
    return std::nullopt;

  nvml::nvmlFieldValue_t field{};
  field.fieldId = fieldId;
  field.scopeId = scopeId;

  if ( m_getFieldValues( *devOpt, 1, &field ) != nvml::NVML_SUCCESS )
    return std::nullopt;
  if ( field.nvmlReturn != nvml::NVML_SUCCESS )
    return std::nullopt;

  switch ( field.valueType )
  {
    case nvml::NVML_VALUE_TYPE_UNSIGNED_LONG:
      return field.value.ulVal;
    case nvml::NVML_VALUE_TYPE_UNSIGNED_INT:
      return static_cast< unsigned long >( field.value.uiVal );
    case nvml::NVML_VALUE_TYPE_UNSIGNED_LONG_LONG:
      return static_cast< unsigned long >( field.value.ullVal );
    default:
      return std::nullopt;
  }
}

std::optional< unsigned int >
NvmlWrapper::getFieldUnsignedInt( unsigned int deviceIndex,
                                  unsigned int fieldId,
                                  unsigned int scopeId ) const noexcept
{
  if ( !m_getFieldValues )
    return std::nullopt;

  auto devOpt = getDevice( deviceIndex );
  if ( !devOpt )
    return std::nullopt;

  nvml::nvmlFieldValue_t field{};
  field.fieldId = fieldId;
  field.scopeId = scopeId;

  if ( m_getFieldValues( *devOpt, 1, &field ) != nvml::NVML_SUCCESS )
    return std::nullopt;
  if ( field.nvmlReturn != nvml::NVML_SUCCESS )
    return std::nullopt;

  switch ( field.valueType )
  {
    case nvml::NVML_VALUE_TYPE_UNSIGNED_INT:
      return field.value.uiVal;
    case nvml::NVML_VALUE_TYPE_UNSIGNED_LONG:
      return static_cast< unsigned int >( field.value.ulVal );
    case nvml::NVML_VALUE_TYPE_UNSIGNED_LONG_LONG:
      return static_cast< unsigned int >( field.value.ullVal );
    default:
      return std::nullopt;
  }
}
