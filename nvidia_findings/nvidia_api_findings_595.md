# NVIDIA API Deep Investigation - Driver 595.45.04 Findings

**System**: Fedora, RTX 5090 Desktop (Blackwell GB202, arch=10), Driver 595.45.04, CUDA 13.2  
**Libraries**: libnvidia-ml.so.595.45.04 (2.6MB, 412 exports), libnvidia-api.so.1 (765KB, 1 export)  
**Previous**: Driver 590.48.01 on RTX 5090 Laptop GPU (2.3MB, 405 exports, 724KB NvAPI)  
**Date**: June 2025

---

## Table of Contents
1. [Summary of Changes](#summary)
2. [NEW: nvmlDeviceGet/SetClockOffsets - Fully Cracked](#clock-offsets)
3. [NVML OC-Relevant Export Availability Matrix](#export-matrix)
4. [Desktop vs Laptop Hardware Differences](#desktop-vs-laptop)
5. [New NVML Exports (595 vs 590)](#new-exports)
6. [Power Smoothing & Workload Power Profile APIs](#power-smoothing)
7. [NvAPI Status on Blackwell Desktop](#nvapi-status)
8. [Fan Control on Desktop](#fan-control)
9. [Power Management](#power-management)
10. [Direct NVML Field Interface (No nvidia-smi)](#direct-field-interface)
11. [P-State & Clock Details](#pstate-clocks)
12. [Internal-Only Functions (Not Exported)](#internal-only)
13. [UCC Integration Recommendations](#ucc-recommendations)

---

## 1. Summary of Changes <a name="summary"></a>

### Driver 590.48.01 → 595.45.04

| Metric | 590.48.01 (Laptop) | 595.45.04 (Desktop) |
|--------|---------------------|---------------------|
| NVML library size | 2.3 MB | 2.6 MB |
| NVML exported `nvml*` symbols | 405 | 412 (+7) |
| NvAPI library size | 724 KB | 765 KB |
| Hardware | RTX 5090 Laptop | RTX 5090 Desktop |
| CUDA cores | 10496 | 21760 |
| Memory bus | 256-bit | 512-bit |
| Max graphics clock | 2400 MHz | 3180 MHz |
| TDP / Power limit | N/A | 600W |
| Fans | 0 | 3 |

### Key Discoveries

1. **`nvmlDeviceGet/SetClockOffsets` — NEW, FULLY CRACKED**: Higher-level clock offset API that returns min/max ranges in the same call. The recommended API for overclocking on Blackwell.
2. **NvAPI is dead on Blackwell**: ClockBoostTable, VFPCurve, Pstates20, Voltage, AllClockFrequencies — all exist in the dispatch table but return NOT_SUPPORTED (-9) for every version/size combination. This applies to both desktop and laptop.
3. **Fan control fully works on desktop**: 3 fans detected, speed/target/policy all functional via NVML.
4. **CoolerInfo now works**: Was NOT_SUPPORTED on laptop, returns valid data on desktop.
5. **7 new NVML exports**: All vGPU-related, no new OC functions.
6. **Power Smoothing / Workload Power Profile**: Exported but NOT_SUPPORTED — likely data center / professional GPU features only.
7. **Direct field-value probing works without `nvidia-smi`**: stable field IDs for power limits and recovery action can be queried via `nvmlDeviceGetFieldValues`.

---

## 2. NEW: nvmlDeviceGet/SetClockOffsets - Fully Cracked <a name="clock-offsets"></a>

### Struct Layout

```c
typedef struct {
    uint32_t version;     // offset 0x00 — version word = 0x01000018
    uint32_t clockType;   // offset 0x04 — 0=Graphics, 2=Memory (1,3-7 = Invalid Argument)
    uint32_t pstate;      // offset 0x08 — P-state index 0-15 (all valid)
    int32_t  offset;      // offset 0x0C — current clock offset in MHz
    int32_t  minOffset;   // offset 0x10 — minimum allowed offset in MHz
    int32_t  maxOffset;   // offset 0x14 — maximum allowed offset in MHz
} nvmlClockOffset_v1_t;   // Total: 24 bytes, version 1
```

**Version word**: `0x01000018` = `(24 & 0x00FFFFFF) | (1 << 24)` = size 24, version 1.

### Clock Type Values

| clockType | Meaning | Offset Range |
|-----------|---------|--------------|
| 0 | Graphics (GPU core) | -1000 to +1000 MHz |
| 1 | Invalid (returns Invalid Argument) | — |
| 2 | Memory | -2000 to +6000 MHz |
| 3-7 | Invalid (returns Invalid Argument) | — |

### P-State Behavior

All P-states (P0-P15) return the same offset range for both GPU and memory clock types. The offset is applied globally, not per-P-state.

### Usage

```c
#include <dlfcn.h>
#include <stdint.h>

typedef unsigned int nvmlReturn_t;
typedef void* nvmlDevice_t;

typedef struct {
    uint32_t version;
    uint32_t clockType;   // 0=GPU, 2=Memory
    uint32_t pstate;      // 0-15
    int32_t  offset;      // current offset MHz
    int32_t  minOffset;   // min allowed offset MHz
    int32_t  maxOffset;   // max allowed offset MHz
} nvmlClockOffset_v1_t;

#define NVML_CLOCK_OFFSET_VERSION_1  0x01000018

// Get current offset and allowed range
nvmlClockOffset_v1_t info = {0};
info.version = NVML_CLOCK_OFFSET_VERSION_1;
info.clockType = 0;  // GPU
info.pstate = 0;     // P0
nvmlReturn_t ret = nvmlDeviceGetClockOffsets(device, &info);
// info.offset = current, info.minOffset = -1000, info.maxOffset = +1000

// Set GPU clock offset to +200 MHz
nvmlClockOffset_v1_t set = {0};
set.version = NVML_CLOCK_OFFSET_VERSION_1;
set.clockType = 0;  // GPU
set.pstate = 0;     // P0
set.offset = 200;   // +200 MHz
ret = nvmlDeviceSetClockOffsets(device, &set);
```

### Comparison with Existing VfOffset APIs

| Feature | GpcClkVfOffset / MemClkVfOffset | Get/SetClockOffsets |
|---------|----------------------------------|---------------------|
| Version word | None (bare int) | 0x01000018 (versioned) |
| Get range info | No (must guess) | Yes (minOffset, maxOffset) |
| Per-P-state | No | Yes (field present, same range for all) |
| Clock type | Separate functions | Single function, clockType field |
| Availability | Exported since ~530 | Exported since ~595 |
| Recommended | Legacy | **Preferred** |

**Recommendation**: Use `Get/SetClockOffsets` as the primary API — it provides range validation in the same call. Fall back to `GpcClkVfOffset`/`MemClkVfOffset` if `GetClockOffsets` is not exported (older drivers).

---

## 3. NVML OC-Relevant Export Availability Matrix <a name="export-matrix"></a>

Tested via `dlsym()` on driver 595.45.04.

### Clock Offset & VF Offset

| Function | Exported | Works |
|----------|----------|-------|
| `nvmlDeviceGetClockOffsets` | ✅ YES | ✅ Returns struct with offset/min/max |
| `nvmlDeviceSetClockOffsets` | ✅ YES | ✅ (same struct) |
| `nvmlDeviceGetGpcClkVfOffset` | ✅ YES | ✅ Returns int offset |
| `nvmlDeviceSetGpcClkVfOffset` | ✅ YES | ✅ |
| `nvmlDeviceGetMemClkVfOffset` | ✅ YES | ✅ Returns int offset |
| `nvmlDeviceSetMemClkVfOffset` | ✅ YES | ✅ |

### Clock Locking

| Function | Exported | Works |
|----------|----------|-------|
| `nvmlDeviceGetGpuLockedClocks` | ❌ NO | — |
| `nvmlDeviceSetGpuLockedClocks` | ✅ YES | Untested |
| `nvmlDeviceResetGpuLockedClocks` | ✅ YES | Untested |
| `nvmlDeviceGetMemoryLockedClocks` | ❌ NO | — |
| `nvmlDeviceSetMemoryLockedClocks` | ✅ YES | Untested |
| `nvmlDeviceResetMemoryLockedClocks` | ✅ YES | Untested |

### Power Management

| Function | Exported | Works |
|----------|----------|-------|
| `nvmlDeviceGetPowerManagementLimit` | ✅ YES | ✅ 600000 mW |
| `nvmlDeviceSetPowerManagementLimit` | ✅ YES | ✅ |
| `nvmlDeviceGetPowerManagementLimitConstraints` | ✅ YES | ✅ 400-600W |
| `nvmlDeviceGetPowerManagementDefaultLimit` | ✅ YES | ✅ 600000 mW |
| `nvmlDeviceGetEnforcedPowerLimit` | ✅ YES | ✅ 600000 mW |
| `nvmlDeviceGetTotalEnergyConsumption` | ✅ YES | ✅ |

### Fan Control

| Function | Exported | Works |
|----------|----------|-------|
| `nvmlDeviceGetNumFans` | ✅ YES | ✅ 3 fans |
| `nvmlDeviceGetFanSpeed` | ✅ YES | ✅ |
| `nvmlDeviceGetFanSpeed_v2` | ✅ YES | ✅ |
| `nvmlDeviceGetTargetFanSpeed` | ✅ YES | ✅ |
| `nvmlDeviceSetFanSpeed_v2` | ✅ YES | ✅ |
| `nvmlDeviceSetDefaultFanSpeed_v2` | ✅ YES | ✅ |
| `nvmlDeviceGetFanControlPolicy_v2` | ✅ YES | ✅ |
| `nvmlDeviceSetFanControlPolicy` | ✅ YES | ✅ |
| `nvmlDeviceGetMinMaxFanSpeed` | ✅ YES | ✅ min=30%, max=100% |
| `nvmlDeviceGetFanSpeedRPM` | ✅ YES | ⚠️ CRASHES for fans 1,2; Invalid for fan 0 |
| `nvmlDeviceGetCoolerInfo` | ✅ YES | ✅ sz=16, v1, count=2 |

### P-State & Clocks

| Function | Exported | Works |
|----------|----------|-------|
| `nvmlDeviceGetPerformanceModes` | ✅ YES | ✅ 5 P-states |
| `nvmlDeviceGetPerformanceState` | ✅ YES | ✅ |
| `nvmlDeviceGetMinMaxClockOfPState` | ✅ YES | ✅ |
| `nvmlDeviceGetClockInfo` | ✅ YES | ✅ |
| `nvmlDeviceGetMaxClockInfo` | ✅ YES | ✅ |
| `nvmlDeviceGetSupportedGraphicsClocks` | ✅ YES | ✅ |
| `nvmlDeviceGetSupportedMemoryClocks` | ✅ YES | ✅ 5 clocks |
| `nvmlDeviceGetApplicationsClock` | ✅ YES | ✅ |
| `nvmlDeviceSetApplicationsClocks` | ✅ YES | Untested |
| `nvmlDeviceResetApplicationsClocks` | ✅ YES | Untested |
| `nvmlDeviceGetMaxCustomerBoostClock` | ✅ YES | ❌ Not Supported |
| `nvmlDeviceGetPstates20` | ❌ NO | — |

### Thermal & Monitoring

| Function | Exported | Works |
|----------|----------|-------|
| `nvmlDeviceGetTemperature` | ✅ YES | ✅ GPU=32°C, Mem=Invalid |
| `nvmlDeviceGetTemperatureThreshold` | ✅ YES | ✅ Shutdown=96, Slow=93, Max=90 |
| `nvmlDeviceSetTemperatureThreshold` | ✅ YES | Untested |
| `nvmlDeviceGetThermalSettings` | ✅ YES | ✅ |
| `nvmlDeviceGetMarginTemperature` | ✅ YES | ✅ 58°C margin |
| `nvmlDeviceGetDynamicPstatesInfo` | ✅ YES | ✅ |
| `nvmlDeviceGetAdaptiveClockInfoStatus` | ✅ YES | ✅ status=1 |
| `nvmlDeviceGetViolationStatus` | ✅ YES | ✅ |
| `nvmlDeviceGetCurrentClocksEventReasons` | ✅ YES | ✅ |
| `nvmlDeviceGetCurrentClocksThrottleReasons` | ✅ YES | ✅ |
| `nvmlDeviceGetPowerMizerMode_v1` | ✅ YES | ✅ |
| `nvmlDeviceGetFieldValues` | ✅ YES | ✅ |

### Power Smoothing (NOT SUPPORTED on consumer GPUs)

| Function | Exported | Works |
|----------|----------|-------|
| `nvmlDevicePowerSmoothingSetState` | ✅ YES | ❌ Not Supported |
| `nvmlDevicePowerSmoothingActivatePresetProfile` | ✅ YES | ❌ Not Supported |
| `nvmlDevicePowerSmoothingUpdatePresetProfileParam` | ✅ YES | ❌ Not Supported |

### Workload Power Profiles (NOT SUPPORTED on consumer GPUs)

| Function | Exported | Works |
|----------|----------|-------|
| `nvmlDeviceWorkloadPowerProfileGetProfilesInfo` | ✅ YES | ❌ Not Supported |
| `nvmlDeviceWorkloadPowerProfileGetCurrentProfiles` | ✅ YES | ❌ Not Supported |
| `nvmlDeviceWorkloadPowerProfileSetRequestedProfiles` | ✅ YES | ❌ Not Supported |
| `nvmlDeviceWorkloadPowerProfileClearRequestedProfiles` | ✅ YES | ❌ Not Supported |
| `nvmlDeviceWorkloadPowerProfileUpdateProfiles_v1` | ✅ YES | ❌ Not Supported |

---

## 4. Desktop vs Laptop Hardware Differences <a name="desktop-vs-laptop"></a>

### GPU Specifications

| Property | RTX 5090 Laptop | RTX 5090 Desktop |
|----------|-----------------|-------------------|
| CUDA cores | 10496 | 21760 |
| Memory bus width | 256-bit | 512-bit |
| Max graphics clock | 2400 MHz | 3180 MHz |
| Max SM clock | 2400 MHz | 3180 MHz |
| Max memory clock | 12001 MHz | 14001 MHz |
| Max video clock | 2310 MHz | 3090 MHz |
| TDP / Power limit | ~150W | 600W |
| Power range | Unknown | 400-600W |
| Fan count | 0 (system-managed) | 3 |
| Temperature shutdown | 96°C | 96°C |
| Temperature slowdown | 93°C | 93°C |
| Temperature GPU max | 90°C | 90°C |

### API Differences

| API | Laptop (590) | Desktop (595) |
|-----|-------------|---------------|
| nvmlDeviceGetCoolerInfo | ❌ Not Supported | ✅ Works (2 coolers) |
| nvmlDeviceGetNumFans | 0 fans | 3 fans |
| Fan speed control | N/A (no fans) | ✅ Fully functional |
| nvmlDeviceGetFanSpeedRPM | N/A | ⚠️ Crashes for fans 1,2 |
| GPU clock offset range | -1000 to +1000 | -1000 to +1000 |
| Memory clock offset range | -2000 to +6000 | -2000 to +6000 |
| Power management | Limited | Full (400-600W) |
| NvAPI OC functions | NOT_SUPPORTED | NOT_SUPPORTED |

### P-State Table (Desktop — from PerformanceModes)

| P-State | GPU Clock Range | Memory Clock | Memory Transfer Rate |
|---------|----------------|--------------|----------------------|
| perf=0 | 180-885 MHz | 405 MHz | 810 MHz |
| perf=1 | 180-3090 MHz | 810 MHz | 1620 MHz |
| perf=2 | 270-3180 MHz | 7001 MHz | 14002 MHz |
| perf=3 | 270-3180 MHz | 13801 MHz | 27602 MHz |
| perf=4 | 270-3180 MHz | (truncated) | (truncated) |

---

## 5. New NVML Exports (595 vs 590) <a name="new-exports"></a>

7 new exports, all vGPU-related:

```
nvmlVgpuInstanceGetGpuPciId
nvmlVgpuInstanceGetLicenseInfo_v2
nvmlVgpuTypeGetGpuInstanceProfileId_v2
nvmlDeviceGetVgpuCapabilities
nvmlVgpuInstanceGetEccMode
nvmlVgpuTypeGetMaxInstancesPerVm
nvmlVgpuTypeGetCapabilities_v2
```

**No new OC-relevant exports were added** between 590 and 595. The `GetClockOffsets`/`SetClockOffsets` functions were already present in 590 but were not investigated at that time.

---

## 6. Power Smoothing & Workload Power Profile APIs <a name="power-smoothing"></a>

### Power Smoothing

Three new functions are exported but return NOT_SUPPORTED on consumer RTX GPUs:

- `nvmlDevicePowerSmoothingSetState` — accepts versioned struct (sz=8, v1, vword=0x01000008)
- `nvmlDevicePowerSmoothingActivatePresetProfile` — accepts versioned struct (sz=24, v1)
- `nvmlDevicePowerSmoothingUpdatePresetProfileParam` — accepts versioned struct (sz=24, v1)

These are likely for NVIDIA data center GPUs (H100, B200, etc.) for managing power delivery in multi-GPU rack configurations.

### Workload Power Profiles

Five new functions are exported but return NOT_SUPPORTED on consumer RTX GPUs:

- `nvmlDeviceWorkloadPowerProfileGetProfilesInfo`
- `nvmlDeviceWorkloadPowerProfileGetCurrentProfiles`
- `nvmlDeviceWorkloadPowerProfileSetRequestedProfiles`
- `nvmlDeviceWorkloadPowerProfileClearRequestedProfiles`
- `nvmlDeviceWorkloadPowerProfileUpdateProfiles_v1`

---

## 7. NvAPI Status on Blackwell Desktop <a name="nvapi-status"></a>

### Dispatch Table Scan

| Function ID | Name | In Dispatch Table | Works |
|-------------|------|-------------------|-------|
| 0xDCB616C3 | GPU_GetAllClockFrequencies | ✅ FOUND | ❌ NOT_SUPPORTED |
| 0x465F9BCF | GPU_GetVoltage | ✅ FOUND | ❌ NOT_SUPPORTED |
| 0x23F1B133 | GPU_GetClockBoostTable | ✅ FOUND | ❌ NOT_SUPPORTED |
| 0x507B4B59 | GPU_GetClockBoostTable_alt | ✅ FOUND | ❌ NOT_SUPPORTED |
| 0x21537AD4 | GPU_GetVFPCurve | ✅ FOUND | ❌ NOT_SUPPORTED |
| 0x6FF81213 | GPU_GetPstates20 | ✅ FOUND | ❌ NOT_SUPPORTED |
| 0x60DED2ED | GPU_ClientPowerTopologyGetInfo | ✅ FOUND | ❌ NOT_SUPPORTED |
| 0x3D358A0C | GPU_PerfPoliciesGetStatus | ✅ FOUND | ❌ NOT_SUPPORTED |
| 0x65FE3AAD | GPU_GetThermalInfo | ✅ FOUND | ❌ NOT_SUPPORTED |
| 0x0D258BB5 | GPU_Unknown_ReferenceClocks | ✅ FOUND | ❌ NOT_SUPPORTED |
| 0x34206D86 | GPU_GetClockTable | ✅ FOUND | ❌ NOT_SUPPORTED |
| 0xAD95F5ED | GPU_ClientPowerPoliciesSetStatus | ✅ FOUND | ❌ NOT_SUPPORTED |
| 0xE9C425A1 | GPU_GetThermalPoliciesStatus | ✅ FOUND | ❌ NOT_SUPPORTED |

### Not in Dispatch Table

| Function ID | Name |
|-------------|------|
| 0x7F5F90A7 | GPU_GetVFPCurve_alt |
| 0x0C0B2B96 | GPU_GetClockBoostLock |
| 0x39944E71 | GPU_GetClockBoostMask |
| 0x74819072 | GPU_SetClockBoostTable |
| 0x0733D009 | GPU_GetClockBoostRanges |
| 0xDA4A8A28 | GPU_ClientPowerPoliciesGetInfo |
| 0x28E5B430 | GPU_GetThermalPoliciesInfo |
| 0x034C0B13 | GPU_SetThermalPoliciesStatus |
| 0x0F4DAC4F | GPU_SetPstates20 |
| 0x1F7B35A2 | GPU_GetPerformanceDecreaseInfo |

### Conclusion

**NvAPI is NOT usable for overclocking on Blackwell architecture.** All OC-relevant function pointers exist in the dispatch table but unconditionally return NOT_SUPPORTED (-9) for every struct version and size combination tested (v1-v4, sizes 4-8192 bytes). This applies to both RTX 5090 Desktop and Laptop.

All NvAPI-based OC functionality has been replaced by NVML equivalents:
- Clock offsets → `nvmlDeviceGet/SetClockOffsets`, `nvmlDeviceGet/SetGpcClkVfOffset`, `nvmlDeviceGet/SetMemClkVfOffset`
- Power limits → `nvmlDeviceGet/SetPowerManagementLimit`
- Fan control → `nvmlDeviceGet/SetFanSpeed_v2`, `nvmlDeviceGet/SetFanControlPolicy`

---

## 8. Fan Control on Desktop <a name="fan-control"></a>

### Fan Topology

- **3 fans detected** via `nvmlDeviceGetNumFans`
- **2 coolers** reported via `nvmlDeviceGetCoolerInfo` (sz=16, v1)
- All fans initially at 0% speed, 30% target (idle, fans off)
- Min/Max fan speed: 30% to 100%

### Fan API Status

| Fan | Speed | Target | RPM | Policy |
|-----|-------|--------|-----|--------|
| 0 | ✅ 0% | ✅ 30% | ❌ Invalid Argument | ✅ 0 |
| 1 | ✅ 0% | ✅ 30% | ⚠️ SIGSEGV (crash) | ✅ 0 |
| 2 | ✅ 0% | ✅ 30% | ⚠️ SIGSEGV (crash) | ✅ 0 |

**Warning**: `nvmlDeviceGetFanSpeedRPM` crashes with SIGSEGV for fan indices 1 and 2. For fan 0, it returns "Invalid Argument". This API should not be used on this hardware. Use `nvmlDeviceGetFanSpeed_v2` / `nvmlDeviceGetTargetFanSpeed` instead.

### CoolerInfo Struct (NEW — works on desktop, was NOT_SUPPORTED on laptop)

```c
struct NvmlCoolerInfo {
    uint32_t version;  // = 0x01000010 (size=16, version=1)
    uint32_t reserved; // padding
    uint32_t count;    // = 2 (number of coolers)
    uint32_t value;    // = 14 (cooling level? unknown semantics)
};
```

---

## 9. Power Management <a name="power-management"></a>

| Property | Value |
|----------|-------|
| Current power limit | 600W |
| Default power limit | 600W |
| Min power limit | 400W |
| Max power limit | 600W |
| Enforced power limit | 600W |
| Idle power draw | ~21.6W |

Power limit can be set in the range 400,000-600,000 mW using `nvmlDeviceSetPowerManagementLimit`.

---

## 10. Direct NVML Field Interface (No nvidia-smi) <a name="direct-field-interface"></a>

`nvidia-smi` was validated as a frontend over NVML plus kernel ioctls. To avoid using the CLI tool, direct `nvmlDeviceGetFieldValues` scanning was performed.

### Stable field IDs observed on this host (driver 595.45.04)

The following IDs were reproducible across repeated scans of IDs 1..8192 and in root/non-root comparison:

| Field ID | Official NVML Constant | Observed Value | Meaning |
|----------|------------------------|----------------|---------|
| 3 | `NVML_FI_DEV_ECC_SBE_VOL_TOTAL` | 0 | Volatile single-bit ECC total |
| 4 | `NVML_FI_DEV_ECC_DBE_VOL_TOTAL` | 0 | Volatile double-bit ECC total |
| 5 | `NVML_FI_DEV_ECC_SBE_AGG_TOTAL` | 0 | Aggregate single-bit ECC total |
| 6 | `NVML_FI_DEV_ECC_DBE_AGG_TOTAL` | 0 | Aggregate double-bit ECC total |
| 147 | `NVML_FI_DEV_NVSWITCH_CONNECTED_LINK_COUNT` | 0 | NVSwitch-connected link count |
| 187 | `NVML_FI_DEV_POWER_MIN_LIMIT` | 400000 | Min power limit (mW) |
| 188 | `NVML_FI_DEV_POWER_MAX_LIMIT` | 600000 | Max power limit (mW) |
| 189 | `NVML_FI_DEV_POWER_DEFAULT_LIMIT` | 600000 | Default power limit (mW) |
| 196 | `NVML_FI_DEV_TEMPERATURE_GPU_MAX_TLIMIT` | zero/empty payload on this host | T.Limit GPU max threshold (Ada+) |
| 230 | `NVML_FI_DEV_GET_GPU_RECOVERY_ACTION` | 1 | GPU recovery action enum |

### Practical notes

- IDs 187/188/189 were only meaningful at `scopeId=0` in this environment.
- IDs 3/4/5/6, 147, 196, and 230 responded across tested scope IDs.
- ID 196 returned success but payload bytes were all zero in this environment, so treat it as ambiguous until validated on another host.
- Earlier exploratory scans occasionally showed additional perf-policy IDs (74-81), but those were not stable in repeat runs and should not be used for production telemetry yet.

### UCC implication

Direct field-value probing is a valid no-CLI fallback path for:

- power limit metadata (`187/188/189`) and
- recovery action telemetry (`230`).

For temperature, clocks, and live power draw, continue to prefer dedicated NVML APIs first.

---

## 11. P-State & Clock Details <a name="pstate-clocks"></a>

### Clock Ranges (from nvmlDeviceGetMinMaxClockOfPState)

| Clock Type | P-State | Min | Max |
|------------|---------|-----|-----|
| Graphics | P0 | 270 MHz | 3180 MHz |
| Graphics | P1 | 270 MHz | 3180 MHz |
| Graphics | P3 | 270 MHz | 3180 MHz |
| SM | P0 | 270 MHz | 3180 MHz |
| SM | P1 | 270 MHz | 3180 MHz |
| SM | P3 | 270 MHz | 3180 MHz |
| Memory | P0 | 14001 MHz | 14001 MHz |
| Memory | P1 | 13801 MHz | 13801 MHz |
| Memory | P3 | 7001 MHz | 7001 MHz |
| Video | P0 | 600 MHz | 3090 MHz |
| Video | P1 | 600 MHz | 3090 MHz |
| Video | P3 | 600 MHz | 3090 MHz |

Note: P2 and P4 return "Unknown Error" — these P-states are not defined on this GPU.

### Max Clock Info

| Type | Speed |
|------|-------|
| Graphics | 3180 MHz |
| SM | 3180 MHz |
| Memory | 14001 MHz |
| Video | 3090 MHz |

---

## 12. Internal-Only Functions (Not Exported) <a name="internal-only"></a>

Found via `strings` analysis of the NVML library — these function names appear internally but are NOT exported (cannot be called via `dlsym`):

| Internal Function | Purpose |
|-------------------|---------|
| `cDeviceGetInstructionAwareVFCurve` | Per-instruction VF curve |
| `cDeviceToggleInstructionAwareVFCurve` | Toggle instruction-aware VF |
| `cDeviceFanCoolerGetControl` | Low-level fan cooler control |
| `cDeviceFanCoolerSetControl` | Low-level fan cooler set |
| `cDeviceGetVoltageMicrovolts` | Read voltage in µV |
| `cDeviceGetVoltage` | Read voltage |
| `cDeviceGetClockMarginMHz` | Clock margin reading |
| `nvmlDeviceGetSupportedClocksOffset` | Supported clock offset query |
| `nvmlDeviceGetClockBoostRange` | Clock boost range info |

These functions exist as code inside the library but are not present in the dynamic symbol table. They cannot be used from user space without binary patching.

---

## 13. UCC Integration Recommendations <a name="ucc-recommendations"></a>

### Priority 1: Implement nvmlDeviceGet/SetClockOffsets

Replace/supplement the existing `GpcClkVfOffset`/`MemClkVfOffset` approach with `Get/SetClockOffsets`:

```cpp
// New approach: query ranges, then set within bounds
struct nvmlClockOffset_v1_t {
    uint32_t version = 0x01000018;
    uint32_t clockType;  // 0=GPU, 2=Mem
    uint32_t pstate;
    int32_t  offset;
    int32_t  minOffset;
    int32_t  maxOffset;
};

// Query GPU clock offset range
nvmlClockOffset_v1_t gpu_info = {.version=0x01000018, .clockType=0, .pstate=0};
nvmlDeviceGetClockOffsets(device, &gpu_info);
// Now gpu_info.minOffset=-1000, gpu_info.maxOffset=+1000

// Set GPU clock offset
nvmlClockOffset_v1_t gpu_set = {.version=0x01000018, .clockType=0, .pstate=0, .offset=200};
nvmlDeviceSetClockOffsets(device, &gpu_set);
```

**Benefits**:
- Range information in a single call (no need to hardcode or guess limits)
- Versioned struct (future-proof)
- Unified API for GPU + Memory clock offsets
- Can check `dlsym("nvmlDeviceGetClockOffsets")` at runtime and fall back to VfOffset APIs

### Priority 2: Add Desktop Fan Control

Desktop GPUs have fans that can be controlled via NVML:
- Set fan speed: `nvmlDeviceSetFanSpeed_v2(device, fan, speed_pct)`
- Set policy: `nvmlDeviceSetFanControlPolicy(device, fan, policy)` (0=auto, 1=manual)
- Reset to auto: `nvmlDeviceSetDefaultFanSpeed_v2(device, fan)`
- Min/max: `nvmlDeviceGetMinMaxFanSpeed(device, &min, &max)` → 30-100%
- **Avoid** `nvmlDeviceGetFanSpeedRPM` — crashes on fan 1,2

### Priority 3: Drop NvAPI OC Code Paths for Blackwell

NvAPI OC functions are completely non-functional on Blackwell (arch=10). When architecture >= 10 is detected, skip NvAPI-based overclocking entirely and use only NVML APIs.

### Priority 4: Add Direct Field-Value Fallbacks (No nvidia-smi subprocess)

Add a lightweight field query path for IDs with stable semantics on consumer Blackwell:

- `NVML_FI_DEV_POWER_MIN_LIMIT` (187)
- `NVML_FI_DEV_POWER_MAX_LIMIT` (188)
- `NVML_FI_DEV_POWER_DEFAULT_LIMIT` (189)
- `NVML_FI_DEV_GET_GPU_RECOVERY_ACTION` (230)

Use these as fallback/augmentation, while keeping dedicated NVML calls as primary data sources.

### Runtime Feature Detection

```cpp
// Check for new ClockOffsets API
auto pfnGetClockOffsets = (decltype(&nvmlDeviceGetClockOffsets))
    dlsym(nvml_handle, "nvmlDeviceGetClockOffsets");
auto pfnSetClockOffsets = (decltype(&nvmlDeviceSetClockOffsets))
    dlsym(nvml_handle, "nvmlDeviceSetClockOffsets");

if (pfnGetClockOffsets && pfnSetClockOffsets) {
    // Use new API with range info
} else {
    // Fall back to GpcClkVfOffset / MemClkVfOffset
}
```

---

## Appendix: Probe Programs

| File | Description |
|------|-------------|
| `deep_probe_595.c` | Comprehensive 19-category NVIDIA API probe |
| `deep_probe_595_output.txt` | Output from deep_probe_595 |
| `deep_oc_probe_595.c` | Focused OC struct analysis (ClockOffsets layout + NvAPI brute-force) |
| `deep_oc_probe_595_output.txt` | Output from deep_oc_probe_595 |
| `field_scan_595.c` | Direct `nvmlDeviceGetFieldValues` scanner |
| `field_scan_595_output.txt` | Non-root field scan output |
| `field_scan_595_output_root.txt` | Root field scan output |

### Version Encoding Reference

```
NVML:  version_word = (struct_size & 0x00FFFFFF) | (version_number << 24)
NvAPI: version_word = (struct_size & 0xFFFF) | (version_number << 16)
```

### Error Code Reference

| Code | NVML | NvAPI |
|------|------|-------|
| 0 | SUCCESS | OK |
| 2 | INVALID_ARGUMENT | — |
| 3 | NOT_SUPPORTED | — |
| 12 | ARGUMENT_VERSION_MISMATCH | — |
| -5 | — | INCOMPATIBLE_STRUCT_VERSION |
| -6 | — | INVALID_HANDLE |
| -9 | — | NOT_SUPPORTED |
