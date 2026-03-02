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

#pragma once

#include <cstdint>
#include <dlfcn.h>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

/**
 * @brief Minimal NVML type definitions for dlopen-based access.
 *
 * These are ABI-compatible with the actual NVML types from nvml.h.
 * We define them here to avoid requiring the CUDA toolkit headers at build time.
 */
namespace nvml
{

using nvmlReturn_t = unsigned int;
using nvmlDevice_t = void*;

static constexpr nvmlReturn_t NVML_SUCCESS = 0;
static constexpr nvmlReturn_t NVML_ERROR_NOT_SUPPORTED = 3;

enum nvmlClockType_t : unsigned int
{
  NVML_CLOCK_GRAPHICS = 0,
  NVML_CLOCK_SM = 1,
  NVML_CLOCK_MEM = 2,
  NVML_CLOCK_VIDEO = 3,
};

enum nvmlPstates_t : unsigned int
{
  NVML_PSTATE_0 = 0,
  NVML_PSTATE_1 = 1,
  NVML_PSTATE_2 = 2,
  NVML_PSTATE_3 = 3,
  NVML_PSTATE_4 = 4,
  NVML_PSTATE_5 = 5,
  NVML_PSTATE_6 = 6,
  NVML_PSTATE_7 = 7,
  NVML_PSTATE_8 = 8,
  NVML_PSTATE_9 = 9,
  NVML_PSTATE_10 = 10,
  NVML_PSTATE_11 = 11,
  NVML_PSTATE_12 = 12,
  NVML_PSTATE_13 = 13,
  NVML_PSTATE_14 = 14,
  NVML_PSTATE_15 = 15,
  NVML_PSTATE_UNKNOWN = 32,
};

// NVML clock offset info structure (v1)
struct nvmlClockOffset_t
{
  unsigned int version;
  nvmlClockType_t type;
  nvmlPstates_t pstate;
  int clockOffsetMHz;
  int minClockOffsetMHz;
  int maxClockOffsetMHz;
};

#define NVML_CLOCK_OFFSET_VER1 \
  ( static_cast< unsigned int >( sizeof( nvml::nvmlClockOffset_t ) ) | ( 1U << 24 ) )

static constexpr unsigned int NVML_MAX_GPU_PERF_PSTATES = 16;

/// Utilization rates returned by nvmlDeviceGetUtilizationRates
struct nvmlUtilization_t
{
  unsigned int gpu;    ///< Compute utilization in percent over the last sample period
  unsigned int memory; ///< Memory utilization in percent over the last sample period
};

/// Device memory information returned by nvmlDeviceGetMemoryInfo(_v2)
struct nvmlMemory_t
{
  unsigned long long total;
  unsigned long long free;
  unsigned long long used;
};

using nvmlClocksThrottleReasons_t = unsigned long long;

static constexpr nvmlClocksThrottleReasons_t NVML_CLOCKS_THROTTLE_REASON_GPU_IDLE = 0x0000000000000001ULL;
static constexpr nvmlClocksThrottleReasons_t NVML_CLOCKS_THROTTLE_REASON_APPLICATIONS_CLOCKS_SETTING = 0x0000000000000002ULL;
static constexpr nvmlClocksThrottleReasons_t NVML_CLOCKS_THROTTLE_REASON_SW_POWER_CAP = 0x0000000000000004ULL;
static constexpr nvmlClocksThrottleReasons_t NVML_CLOCKS_THROTTLE_REASON_HW_SLOWDOWN = 0x0000000000000008ULL;
static constexpr nvmlClocksThrottleReasons_t NVML_CLOCKS_THROTTLE_REASON_SYNC_BOOST = 0x0000000000000010ULL;
static constexpr nvmlClocksThrottleReasons_t NVML_CLOCKS_THROTTLE_REASON_SW_THERMAL_SLOWDOWN = 0x0000000000000020ULL;
static constexpr nvmlClocksThrottleReasons_t NVML_CLOCKS_THROTTLE_REASON_HW_THERMAL_SLOWDOWN = 0x0000000000000040ULL;
static constexpr nvmlClocksThrottleReasons_t NVML_CLOCKS_THROTTLE_REASON_HW_POWER_BRAKE_SLOWDOWN = 0x0000000000000080ULL;
static constexpr nvmlClocksThrottleReasons_t NVML_CLOCKS_THROTTLE_REASON_DISPLAY_CLOCK_SETTING = 0x0000000000000100ULL;

} // namespace nvml

/**
 * @brief P-State info for a single clock type at a single P-state.
 */
struct NvmlPStateClockInfo
{
  unsigned int pstate;     ///< P-state index (0–15)
  unsigned int minMHz;     ///< Minimum clock in MHz
  unsigned int maxMHz;     ///< Maximum clock in MHz
  int currentOffset;       ///< Currently applied clock offset in MHz
  int minOffset;           ///< Minimum allowed clock offset in MHz
  int maxOffset;           ///< Maximum allowed clock offset in MHz
  bool offsetWritable = false; ///< Whether writing offset is supported for this clock/P-state
};

/**
 * @brief Complete GPU OC state read from NVML.
 */
struct NvmlOCState
{
  std::string gpuName;
  std::string pciBusId;

  std::vector< NvmlPStateClockInfo > gpuPStates;   ///< Core clock P-states
  std::vector< NvmlPStateClockInfo > vramPStates;   ///< VRAM clock P-states

  // Locked clocks (set by user, tracked locally)
  std::optional< std::pair< unsigned int, unsigned int > > gpuLockedClocks;
  std::optional< std::pair< unsigned int, unsigned int > > vramLockedClocks;

  // Overall clock ranges (min across all pstates .. max across all pstates)
  std::optional< std::pair< unsigned int, unsigned int > > gpuClockRange;
  std::optional< std::pair< unsigned int, unsigned int > > vramClockRange;

  // Power info
  double powerDrawW     = 0.0;
  double powerLimitW    = 0.0;
  double powerMaxW      = 0.0;
  double powerDefaultW  = 0.0;
  double powerMinW      = 0.0;

  // Temperature
  unsigned int tempC = 0;
  unsigned int tempShutdownC = 0;

  bool offsetsSupported = false;
  bool lockedClocksSupported = false;
};

/**
 * @brief Runtime NVML wrapper using dlopen/dlsym.
 *
 * Provides GPU overclocking functionality without requiring NVML headers
 * at compile time. The library (libnvidia-ml.so.1) is loaded dynamically.
 */
class NvmlWrapper
{
public:
  explicit NvmlWrapper( bool enableOcFeatures = true );
  ~NvmlWrapper();

  // Non-copyable, non-movable
  NvmlWrapper( const NvmlWrapper& ) = delete;
  NvmlWrapper& operator=( const NvmlWrapper& ) = delete;

  /**
   * @brief Check if NVML was loaded and initialized successfully.
   */
  [[nodiscard]] bool isAvailable() const noexcept { return m_initialized; }

  /**
   * @brief Get the number of NVIDIA GPUs found.
   */
  [[nodiscard]] unsigned int deviceCount() const noexcept { return m_deviceCount; }

  /**
   * @brief Read complete OC state for a GPU by index.
   */
  [[nodiscard]] std::optional< NvmlOCState > getOCState( unsigned int deviceIndex ) const;

  /**
   * @brief Set a clock offset for a specific clock type and P-state.
   * @return true on success
   */
  bool setClockOffset( unsigned int deviceIndex,
                       nvml::nvmlClockType_t clockType,
                       nvml::nvmlPstates_t pstate,
                       int offsetMHz );

  /**
   * @brief Set GPU locked clocks (min/max core clock range).
   * @return true on success
   */
  bool setGpuLockedClocks( unsigned int deviceIndex,
                           unsigned int minMHz,
                           unsigned int maxMHz );

  /**
   * @brief Set VRAM locked clocks (min/max memory clock range).
   * @return true on success
   */
  bool setVramLockedClocks( unsigned int deviceIndex,
                            unsigned int minMHz,
                            unsigned int maxMHz );

  /**
   * @brief Reset GPU locked clocks to default.
   * @return true on success
   */
  bool resetGpuLockedClocks( unsigned int deviceIndex );

  /**
   * @brief Reset VRAM locked clocks to default.
   * @return true on success
   */
  bool resetVramLockedClocks( unsigned int deviceIndex );

  /**
   * @brief Reset all clock offsets to 0.
   * @return true on success
   */
  bool resetAllClockOffsets( unsigned int deviceIndex );

  /**
   * @brief Set power management limit in milliwatts.
   * @return true on success
   */
  bool setPowerLimit( unsigned int deviceIndex, unsigned int milliwatts );

  /**
   * @brief Reset power limit to default.
   * @return true on success
   */
  bool resetPowerLimit( unsigned int deviceIndex );

  /** @brief Whether offset writes are known to be writable for this clock/P-state. */
  [[nodiscard]] bool isClockOffsetWritable( unsigned int deviceIndex,
                                            nvml::nvmlClockType_t clockType,
                                            nvml::nvmlPstates_t pstate ) const;

  // ---- Live monitoring getters (replace nvidia-smi subprocess calls) ----

  /** @brief GPU temperature in °C. */
  [[nodiscard]] std::optional< unsigned int > getTemperatureDegC( unsigned int deviceIndex ) const noexcept;

  /** @brief Power draw in watts. */
  [[nodiscard]] std::optional< double > getPowerDrawW( unsigned int deviceIndex ) const noexcept;

  /** @brief Enforced (actual) power limit in watts. */
  [[nodiscard]] std::optional< double > getEnforcedPowerLimitW( unsigned int deviceIndex ) const noexcept;

  /** @brief Maximum allowed power limit in watts. */
  [[nodiscard]] std::optional< double > getPowerMaxLimitW( unsigned int deviceIndex ) const noexcept;

  /** @brief Default (factory) power limit in watts. */
  [[nodiscard]] std::optional< double > getPowerDefaultLimitW( unsigned int deviceIndex ) const noexcept;

  /** @brief Current graphics clock in MHz. */
  [[nodiscard]] std::optional< unsigned int > getGpuClockMHz( unsigned int deviceIndex ) const noexcept;

  /** @brief Maximum (boost) graphics clock in MHz. */
  [[nodiscard]] std::optional< unsigned int > getMaxGpuClockMHz( unsigned int deviceIndex ) const noexcept;

  /** @brief Current memory clock in MHz. */
  [[nodiscard]] std::optional< unsigned int > getMemClockMHz( unsigned int deviceIndex ) const noexcept;

  /** @brief Current core voltage in mV (via NvAPI), if available. */
  [[nodiscard]] std::optional< unsigned int > getCoreVoltageMv( unsigned int deviceIndex ) const noexcept;

  /** @brief GPU compute utilization in percent (0–100). */
  [[nodiscard]] std::optional< unsigned int > getComputeUtilPct( unsigned int deviceIndex ) const noexcept;

  /** @brief GPU memory-controller utilization in percent (0–100). */
  [[nodiscard]] std::optional< unsigned int > getMemoryUtilPct( unsigned int deviceIndex ) const noexcept;

  /** @brief Used VRAM in MiB. */
  [[nodiscard]] std::optional< unsigned int > getVramUsedMiB( unsigned int deviceIndex ) const noexcept;

  /** @brief Total VRAM in MiB. */
  [[nodiscard]] std::optional< unsigned int > getVramTotalMiB( unsigned int deviceIndex ) const noexcept;

  /** @brief Current dominant performance-cap / throttle reason. */
  [[nodiscard]] std::optional< std::string > getPerfLimitReason( unsigned int deviceIndex ) const noexcept;

  /** @brief NVENC utilization in percent (0–100). */
  [[nodiscard]] std::optional< unsigned int > getEncoderUtilPct( unsigned int deviceIndex ) const noexcept;

  /** @brief NVDEC utilization in percent (0–100). */
  [[nodiscard]] std::optional< unsigned int > getDecoderUtilPct( unsigned int deviceIndex ) const noexcept;

  /** @brief Current P-state index (0 = P0 maximum, 15 = P15 minimum). */
  [[nodiscard]] std::optional< unsigned int > getCurrentPstate( unsigned int deviceIndex ) const noexcept;

  /** @brief Current graphics-clock offset in MHz at the current P-state. */
  [[nodiscard]] std::optional< int > getGrClockOffsetMHz( unsigned int deviceIndex ) const noexcept;

  /** @brief Current memory-clock offset in MHz at the current P-state. */
  [[nodiscard]] std::optional< int > getMemClockOffsetMHz( unsigned int deviceIndex ) const noexcept;

  /** @brief Returns true if the NVML library was loaded and initialized. */
  bool isInitialized() const { return m_initialized; }

private:
  struct NvApiVoltage
  {
    uint32_t version;
    uint32_t flags;
    uint32_t padding[8];
    uint32_t valueUv;
    uint32_t padding2[8];
  };

  void* m_lib = nullptr;
  void* m_nvapiLib = nullptr;
  bool m_initialized = false;
  bool m_nvapiInitialized = false;
  bool m_enableOcFeatures = true;
  unsigned int m_deviceCount = 0;
  std::vector< void * > m_nvapiGpuHandles;

  // Tracked state for locked clocks (NVML doesn't report these back)
  mutable std::map< unsigned int, std::pair< unsigned int, unsigned int > > m_appliedGpuLockedClocks;
  mutable std::map< unsigned int, std::pair< unsigned int, unsigned int > > m_appliedVramLockedClocks;
  mutable std::map< unsigned int, std::map< int /*clockType*pstate*/, int > > m_appliedOffsets;
  std::map< unsigned int, std::map< int /*clockType*pstate*/, bool > > m_writableOffsets;
  std::map< unsigned int, std::vector< nvml::nvmlPstates_t > > m_supportedPstates;

  // Function pointer types
  using InitFn = nvml::nvmlReturn_t ( * )();
  using ShutdownFn = nvml::nvmlReturn_t ( * )();
  using DeviceGetCountFn = nvml::nvmlReturn_t ( * )( unsigned int* );
  using DeviceGetHandleByIndexFn = nvml::nvmlReturn_t ( * )( unsigned int, nvml::nvmlDevice_t* );
  using DeviceGetNameFn = nvml::nvmlReturn_t ( * )( nvml::nvmlDevice_t, char*, unsigned int );
  using DeviceGetPciBusIdFn = nvml::nvmlReturn_t ( * )( nvml::nvmlDevice_t, char*, unsigned int );
  using DeviceGetSupportedPstatesFn = nvml::nvmlReturn_t ( * )( nvml::nvmlDevice_t, nvml::nvmlPstates_t*, unsigned int );
  using DeviceGetMinMaxClockFn = nvml::nvmlReturn_t ( * )( nvml::nvmlDevice_t, nvml::nvmlClockType_t, nvml::nvmlPstates_t, unsigned int*, unsigned int* );
  using DeviceGetClockOffsetsFn = nvml::nvmlReturn_t ( * )( nvml::nvmlDevice_t, nvml::nvmlClockOffset_t* );
  using DeviceSetClockOffsetsFn = nvml::nvmlReturn_t ( * )( nvml::nvmlDevice_t, nvml::nvmlClockOffset_t* );
  using DeviceSetGpuLockedClocksFn = nvml::nvmlReturn_t ( * )( nvml::nvmlDevice_t, unsigned int, unsigned int );
  using DeviceSetMemLockedClocksFn = nvml::nvmlReturn_t ( * )( nvml::nvmlDevice_t, unsigned int, unsigned int );
  using DeviceResetGpuLockedClocksFn = nvml::nvmlReturn_t ( * )( nvml::nvmlDevice_t );
  using DeviceResetMemLockedClocksFn = nvml::nvmlReturn_t ( * )( nvml::nvmlDevice_t );
  using DeviceGetTemperatureFn = nvml::nvmlReturn_t ( * )( nvml::nvmlDevice_t, unsigned int, unsigned int* );
  using DeviceGetTemperatureThresholdFn = nvml::nvmlReturn_t ( * )( nvml::nvmlDevice_t, unsigned int, unsigned int* );
  using DeviceGetPowerUsageFn = nvml::nvmlReturn_t ( * )( nvml::nvmlDevice_t, unsigned int* );
  using DeviceGetPowerManagementLimitFn = nvml::nvmlReturn_t ( * )( nvml::nvmlDevice_t, unsigned int* );
  using DeviceSetPowerManagementLimitFn = nvml::nvmlReturn_t ( * )( nvml::nvmlDevice_t, unsigned int );
  using DeviceGetPowerManagementLimitConstraintsFn = nvml::nvmlReturn_t ( * )( nvml::nvmlDevice_t, unsigned int*, unsigned int* );
  using DeviceGetPowerManagementDefaultLimitFn = nvml::nvmlReturn_t ( * )( nvml::nvmlDevice_t, unsigned int* );
  using DeviceGetPerformanceStateFn = nvml::nvmlReturn_t ( * )( nvml::nvmlDevice_t, nvml::nvmlPstates_t* );
  using DeviceGetClockInfoFn = nvml::nvmlReturn_t ( * )( nvml::nvmlDevice_t, unsigned int, unsigned int* );
  using DeviceGetMaxClockInfoFn = nvml::nvmlReturn_t ( * )( nvml::nvmlDevice_t, unsigned int, unsigned int* );
  using DeviceGetEnforcedPowerLimitFn = nvml::nvmlReturn_t ( * )( nvml::nvmlDevice_t, unsigned int* );
  using DeviceGetUtilizationRatesFn = nvml::nvmlReturn_t ( * )( nvml::nvmlDevice_t, nvml::nvmlUtilization_t* );
  using DeviceGetMemoryInfoFn = nvml::nvmlReturn_t ( * )( nvml::nvmlDevice_t, nvml::nvmlMemory_t* );
  using DeviceGetCurrentClocksThrottleReasonsFn = nvml::nvmlReturn_t ( * )( nvml::nvmlDevice_t, nvml::nvmlClocksThrottleReasons_t* );
  using DeviceGetEncoderUtilizationFn = nvml::nvmlReturn_t ( * )( nvml::nvmlDevice_t, unsigned int*, unsigned int* );
  using DeviceGetDecoderUtilizationFn = nvml::nvmlReturn_t ( * )( nvml::nvmlDevice_t, unsigned int*, unsigned int* );

  using NvApiQueryInterfaceFn = void * ( * )( uint32_t );
  using NvApiInitializeFn = int32_t ( * )( void );
  using NvApiUnloadFn = int32_t ( * )( void );
  using NvApiEnumPhysicalGPUsFn = int32_t ( * )( void *handles[64], uint32_t *count );
  using NvApiGetVoltageFn = int32_t ( * )( void *handle, NvApiVoltage *data );

  // Function pointers (loaded via dlsym)
  InitFn m_init = nullptr;
  ShutdownFn m_shutdown = nullptr;
  DeviceGetCountFn m_getCount = nullptr;
  DeviceGetHandleByIndexFn m_getHandle = nullptr;
  DeviceGetNameFn m_getName = nullptr;
  DeviceGetPciBusIdFn m_getPciBusId = nullptr;
  DeviceGetSupportedPstatesFn m_getSupportedPstates = nullptr;
  DeviceGetMinMaxClockFn m_getMinMaxClock = nullptr;
  DeviceGetClockOffsetsFn m_getClockOffsets = nullptr;
  DeviceSetClockOffsetsFn m_setClockOffsets = nullptr;
  DeviceSetGpuLockedClocksFn m_setGpuLockedClocks = nullptr;
  DeviceSetMemLockedClocksFn m_setMemLockedClocks = nullptr;
  DeviceResetGpuLockedClocksFn m_resetGpuLockedClocks = nullptr;
  DeviceResetMemLockedClocksFn m_resetMemLockedClocks = nullptr;
  DeviceGetTemperatureFn m_getTemperature = nullptr;
  DeviceGetTemperatureThresholdFn m_getTemperatureThreshold = nullptr;
  DeviceGetPowerUsageFn m_getPowerUsage = nullptr;
  DeviceGetPowerManagementLimitFn m_getPowerLimit = nullptr;
  DeviceSetPowerManagementLimitFn m_setPowerLimit = nullptr;
  DeviceGetPowerManagementLimitConstraintsFn m_getPowerLimitConstraints = nullptr;
  DeviceGetPowerManagementDefaultLimitFn m_getPowerLimitDefault = nullptr;
  DeviceGetPerformanceStateFn m_getPerformanceState = nullptr;
  DeviceGetClockInfoFn m_getClockInfo = nullptr;
  DeviceGetMaxClockInfoFn m_getMaxClockInfo = nullptr;
  DeviceGetEnforcedPowerLimitFn m_getEnforcedPowerLimit = nullptr;
  DeviceGetUtilizationRatesFn m_getUtilizationRates = nullptr;
  DeviceGetMemoryInfoFn m_getMemoryInfo = nullptr;
  DeviceGetCurrentClocksThrottleReasonsFn m_getCurrentClocksThrottleReasons = nullptr;
  DeviceGetEncoderUtilizationFn m_getEncoderUtilization = nullptr;
  DeviceGetDecoderUtilizationFn m_getDecoderUtilization = nullptr;

  NvApiQueryInterfaceFn m_nvapiQueryInterface = nullptr;
  NvApiInitializeFn m_nvapiInitialize = nullptr;
  NvApiUnloadFn m_nvapiUnload = nullptr;
  NvApiEnumPhysicalGPUsFn m_nvapiEnumPhysicalGpus = nullptr;
  NvApiGetVoltageFn m_nvapiGetVoltage = nullptr;

  /**
   * @brief Load a function pointer from the NVML library.
   */
  template< typename T >
  T loadSym( const char* name )
  {
    auto* ptr = reinterpret_cast< T >( dlsym( m_lib, name ) );
    if ( !ptr )
      std::cerr << "[NvmlWrapper] Could not load symbol: " << name << std::endl;
    return ptr;
  }

  /**
   * @brief Get a device handle by index.
   */
  [[nodiscard]] std::optional< nvml::nvmlDevice_t > getDevice( unsigned int index ) const;

  void cacheSupportedPstates();
  void initNvapi();
  void probeWritableOffsetPstates();
};
