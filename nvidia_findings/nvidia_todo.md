# NVIDIA UCC Integration TODO

Based on driver 595.45.04 deep probe findings (RTX 5090 Desktop, Blackwell arch=10).  
Reference: `nvidia_findings/nvidia_api_findings_595.md`

---

## Priority 1: Desktop GPU Fan Control via NVML

**Status**: Not implemented  
**Effort**: Medium  
**Files**: `uccd/inc/NvmlWrapper.hpp`, `uccd/src/NvmlWrapper.cpp`, new `NvidiaGpuFanProvider`

UCC currently only has water-cooler fan control (BLE). Desktop NVIDIA GPUs expose full fan control via NVML.

### APIs to integrate

```cpp
// Query
nvmlDeviceGetNumFans(device, &numFans);              // 3 on RTX 5090 Desktop
nvmlDeviceGetFanSpeed_v2(device, fanIdx, &speedPct); // per-fan current %
nvmlDeviceGetTargetFanSpeed(device, fanIdx, &target); // per-fan target %
nvmlDeviceGetMinMaxFanSpeed(device, &minPct, &maxPct); // 30–100%
nvmlDeviceGetFanControlPolicy_v2(device, fanIdx, &policy); // 0=auto, 1=manual

// Control
nvmlDeviceSetFanControlPolicy(device, fanIdx, policy); // 0=auto, 1=manual
nvmlDeviceSetFanSpeed_v2(device, fanIdx, speedPct);    // set manual speed
nvmlDeviceSetDefaultFanSpeed_v2(device, fanIdx);       // restore auto
```

### DANGER: Do NOT use `nvmlDeviceGetFanSpeedRPM`
- Returns "Invalid Argument" for fan 0
- **CRASHES with SIGSEGV** for fans 1 and 2
- Use `nvmlDeviceGetFanSpeed_v2` (percent) instead

### Implementation plan
1. Add fan query/set methods to `NvmlWrapper`
2. Create `NvidiaGpuFanProvider` implementing `IFanProvider`
3. Register in `HardwareManager` during detect
4. Expose via D-Bus (`GetGpuFanSpeed`, `SetGpuFanSpeed`, `SetGpuFanPolicy`)
5. Add GPU fan curve to profile system (like existing CPU fan zones)
6. GUI: fan speed display + manual override slider

### CoolerInfo (supplementary)
```c
// Works on desktop, NOT_SUPPORTED on laptop
// Struct: 16 bytes, version 1, vword=0x01000010
struct NvmlCoolerInfo {
    uint32_t version;  // 0x01000010
    uint32_t reserved;
    uint32_t count;    // 2 (number of coolers)
    uint32_t value;    // 14 (unknown semantics — cooling level?)
};
```

---

## Priority 2: Adaptive NvmlOffsetCaps (Desktop vs Laptop)

**Status**: Hardcoded laptop-safe values  
**Effort**: Small  
**Files**: `uccd/inc/NvmlWrapper.hpp` (NvmlOffsetCaps struct), `uccd/src/NvmlWrapper.cpp`

Current caps at `NvmlWrapper.hpp:150`:
```cpp
struct NvmlOffsetCaps {
    static constexpr int GPU_MIN_OFFSET  = -250;
    static constexpr int GPU_MAX_OFFSET  =  700;
    static constexpr int VRAM_MIN_OFFSET = -500;
    static constexpr int VRAM_MAX_OFFSET = 1000;
};
```

Hardware-reported ranges (both laptop and desktop):
- GPU: -1000 to +1000 MHz
- VRAM: -2000 to +6000 MHz

### Proposal
Make caps adaptive based on hardware-reported ranges from `GetClockOffsets`, applying a safety margin:

```cpp
// Option A: Use hardware ranges directly (clamped to safe percentages)
// Desktop with good cooling: allow 90% of hw range
// Laptop: allow 70% of hw range

// Option B: Detect desktop by fan count or power limit
unsigned int numFans;
nvmlDeviceGetNumFans(device, &numFans);
bool isDesktop = (numFans > 0);
// OR: power limit > 300W suggests desktop

// Desktop suggested caps:
//   GPU: -500 to +900
//   VRAM: -1000 to +2000

// Laptop caps (current, keep as-is):
//   GPU: -250 to +700
//   VRAM: -500 to +1000
```

### Detection heuristic
- `numFans > 0` → desktop (laptops report 0 via NVML, fans are EC-managed)
- `powerMaxLimit > 300W` → desktop
- Either condition → use wider caps

---

## Priority 3: Skip NvAPI on Blackwell (arch ≥ 10)

**Status**: NvAPI loads and probes but all calls return NOT_SUPPORTED  
**Effort**: Small  
**Files**: `uccd/src/NvmlWrapper.cpp` (constructor / NvAPI init section)

All NvAPI OC functions exist in the dispatch table but return -9 (NOT_SUPPORTED) on Blackwell:
- `GPU_GetVoltage` (0x465F9BCF)
- `GPU_GetAllClockFrequencies` (0xDCB616C3)
- `GPU_GetClockBoostTable` (0x23F1B133)
- `GPU_GetVFPCurve` (0x21537AD4)
- `GPU_GetPstates20` (0x6FF81213)
- `GPU_PerfPoliciesGetStatus` (0x3D358A0C)
- `GPU_GetThermalPoliciesStatus` (0xE9C425A1)
- `GPU_ClientPowerTopologyGetInfo` (0x60DED2ED)

### Implementation
```cpp
// After NVML init, check architecture
unsigned int arch = 0;
if (m_getArchitecture && m_getArchitecture(device, &arch) == nvml::NVML_SUCCESS) {
    if (arch >= 10) { // Blackwell+
        // Skip NvAPI dlopen/init entirely
        // Log: "Blackwell GPU detected (arch=10), skipping NvAPI (not supported)"
        m_nvapiInitialized = false;
        return;
    }
}
```

Benefits:
- Eliminates ~8 futile NvAPI calls per monitoring cycle
- Cleaner logs (no NOT_SUPPORTED noise)
- Faster startup

---

## Priority 4: Thermal Margin Monitoring

**Status**: API works, not integrated  
**Effort**: Small  
**Files**: `uccd/inc/NvmlWrapper.hpp`, `uccd/src/NvmlWrapper.cpp`

### API
```cpp
// nvmlDeviceGetMarginTemperature — returns distance to thermal limit
unsigned int margin;
nvmlDeviceGetMarginTemperature(device, &margin); // 58°C at idle
```

### Uses in UCC
- Auto-OC worker: abort if margin < 5°C (thermal safety cutoff)
- Auto-undervolt worker: factor margin into convergence decisions
- GUI: show "thermal headroom" alongside temperature
- Monitoring: track margin over time in metrics history

### Temperature thresholds (RTX 5090)
- Shutdown: 96°C
- Slowdown: 93°C
- GPU Max: 90°C
- Current margin at idle: 58°C (= 90 - 32)

---

## Priority 5: Direct NVML Field-Value Path (No nvidia-smi subprocess)

**Status**: Scanner completed, integration pending  
**Effort**: Small/Medium  
**Files**: `uccd/inc/NvmlWrapper.hpp`, `uccd/src/NvmlWrapper.cpp`, D-Bus payload structs

Goal: expose field-based telemetry directly via `nvmlDeviceGetFieldValues` where stable, so diagnostics can remain CLI-free.

### Stable IDs verified on this host

- `187` = `NVML_FI_DEV_POWER_MIN_LIMIT` (400000 mW)
- `188` = `NVML_FI_DEV_POWER_MAX_LIMIT` (600000 mW)
- `189` = `NVML_FI_DEV_POWER_DEFAULT_LIMIT` (600000 mW)
- `230` = `NVML_FI_DEV_GET_GPU_RECOVERY_ACTION` (value `1` here)

### Implementation plan

1. Add a batched field query helper in `NvmlWrapper`:

```cpp
bool NvmlWrapper::GetFieldValues(const std::vector<unsigned int>& ids,
                                 std::vector<nvmlFieldValue_t>& out,
                                 unsigned int scopeId = 0);
```

2. Add typed wrappers for stable IDs (min/max/default power limit, recovery action).
3. Wire into diagnostics payload and GUI monitoring model as optional metadata.
4. Keep dedicated NVML APIs as primary path (`GetPowerManagementLimit*`, etc.); use field values as fallback/augmentation.

### Caution

- ID `196` (`NVML_FI_DEV_TEMPERATURE_GPU_MAX_TLIMIT`) returned success but zero/empty payload in this environment; do not depend on it yet.
- Earlier one-off scans showed extra IDs (74-81), but these were not stable; do not promote them until repeatable.

---

## Priority 6: Monitor Future Driver Exports

**Status**: Tracking only  
**Effort**: None (periodic checks)

### Internal functions to watch for in future drivers

| Function | Why It Matters |
|----------|---------------|
| `nvmlDeviceGetVoltageMicrovolts` | Replace broken NvAPI voltage reading on Blackwell |
| `nvmlDeviceGetVoltage` | Same — voltage monitoring without NvAPI |
| `nvmlDeviceGetClockMarginMHz` | Clock headroom — useful for auto-OC convergence |
| `nvmlDeviceGetInstructionAwareVFCurve` | Per-instruction VF curve for advanced undervolting |
| `nvmlDeviceToggleInstructionAwareVFCurve` | Toggle instruction-aware VF optimization |
| `nvmlDeviceGetSupportedClocksOffset` | Formal query for offset support |
| `nvmlDeviceGetClockBoostRange` | Boost range info |
| `nvmlDeviceFanCoolerGetControl` | Low-level cooler control |

### Check method
```bash
nm -D /usr/lib/libnvidia-ml.so.1 | grep -i "voltage\|VFCurve\|ClockMargin\|FanCooler\|SupportedClocksOffset\|ClockBoostRange"
```

---

## Not Applicable / No Action

### Power Smoothing APIs
Exported but NOT_SUPPORTED on consumer GPUs. Data center only (H100/B200).
- `nvmlDevicePowerSmoothingSetState`
- `nvmlDevicePowerSmoothingActivatePresetProfile`
- `nvmlDevicePowerSmoothingUpdatePresetProfileParam`

### Workload Power Profile APIs
Exported but NOT_SUPPORTED on consumer GPUs. Data center only.
- `nvmlDeviceWorkloadPowerProfileGetProfilesInfo`
- `nvmlDeviceWorkloadPowerProfileGetCurrentProfiles`
- `nvmlDeviceWorkloadPowerProfileSetRequestedProfiles`
- `nvmlDeviceWorkloadPowerProfileClearRequestedProfiles`
- `nvmlDeviceWorkloadPowerProfileUpdateProfiles_v1`

### New vGPU exports (7 functions)
All virtualization-related, no OC relevance.

---

## Reference: Complete Working API Matrix

See `nvidia_findings/nvidia_api_findings_595.md` Section 3 for the full export availability matrix with 60+ functions tested.

### Key struct: nvmlClockOffset_t (already in UCC)
```c
typedef struct {
    uint32_t version;     // 0x01000018
    uint32_t clockType;   // 0=GPU, 2=Memory
    uint32_t pstate;      // 0–15
    int32_t  offset;      // current offset MHz
    int32_t  minOffset;   // min allowed (-1000 GPU, -2000 Mem)
    int32_t  maxOffset;   // max allowed (+1000 GPU, +6000 Mem)
} nvmlClockOffset_v1_t;
```
