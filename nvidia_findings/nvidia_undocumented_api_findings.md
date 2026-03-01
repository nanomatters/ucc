# NVIDIA Undocumented API Deep Investigation - Complete Findings

**System**: Fedora 45, RTX 5090 Laptop GPU (Blackwell, arch=10), Driver 590.48.01  
**Libraries**: libnvidia-ml.so.590.48.01 (2.3MB, 405 exports), libnvidia-api.so.1 (724KB, 1 export)  
**Date**: June 2025

---

## Table of Contents
1. [Methodology](#methodology)
2. [NVML Undocumented Functions - Cracked](#nvml-cracked)
3. [NVML Undocumented Functions - Unsupported](#nvml-unsupported)
4. [NVML Additional Discoveries](#nvml-additional)
5. [NvAPI Undocumented Functions - Working](#nvapi-working)
6. [NvAPI Undocumented Functions - Unavailable on Blackwell](#nvapi-unavailable)
7. [LACT Cross-Reference](#lact-comparison)
8. [Disassembly Insights](#disassembly)
9. [UCC Integration Recommendations](#ucc-integration)
10. [Version Encoding Reference](#version-encoding)

---

## 1. Methodology <a name="methodology"></a>

### Approach
- **Symbol enumeration**: `nm -D` / `readelf -s` on both libraries
- **Disassembly**: `objdump -d` of function entry points to understand dispatch patterns
- **String extraction**: `strings` on libraries for embedded function names and signatures
- **Brute-force probing**: Systematic version/size scanning with signal handlers for crash protection
- **NVIDIA kernel source**: Extracted from kmod RPM (`/tmp/nvidia-src/`)
- **LACT source comparison**: Fetched from `github.com/ilya-zlobintsev/LACT`

### Version Word Encoding
- **NVML**: `version_word = (struct_size & 0x00FFFFFF) | (version_number << 24)`
- **NvAPI**: `version_word = (struct_size & 0xFFFF) | (version_number << 16)`

---

## 2. NVML Undocumented Functions - CRACKED <a name="nvml-cracked"></a>

### 2.1 nvmlDeviceGetPerformanceModes ⭐

| Property | Value |
|----------|-------|
| **Symbol** | `nvmlDeviceGetPerformanceModes` |
| **Signature** | `nvmlReturn_t (nvmlDevice_t device, void *buf)` |
| **Struct Size** | 2052 bytes |
| **Version** | 1 |
| **Version Word** | `0x01000804` |

**Struct Layout:**
```c
struct NvmlPerformanceModes {
    uint32_t version;       // = 0x01000804
    char text[2048];        // Semicolon-delimited text P-state table
};
```

**Output Format** (semicolon-delimited key=value entries):
```
numPstates=5;
pstate[0].perf=0;pstate[0].nvclock.min=210000;pstate[0].nvclock.max=2400000;pstate[0].nvclock.editable=0;
pstate[0].memclock.min=202000;pstate[0].memclock.max=1750125;pstate[0].memclock.editable=0;
pstate[0].memTransferRate.min=404000;pstate[0].memTransferRate.max=14001000;pstate[0].memTransferRate.editable=0;
pstate[1].perf=1;pstate[1].nvclock.min=210000;pstate[1].nvclock.max=2400000;...
pstate[2].perf=2;...
pstate[3].perf=3;...
pstate[4].perf=4;...
```

**Values are in kHz.** 5 P-states found on RTX 5090 Laptop:
- P0: GPU 210-2400 MHz, Mem 202-1750 MHz, MemTransfer 404-14001 MHz  
- All marked as `editable=0` (read-only)

**Use case**: Getting full P-state frequency ranges including min/max for each performance level.

---

### 2.2 nvmlDeviceGetMarginTemperature ⭐

| Property | Value |
|----------|-------|
| **Symbol** | `nvmlDeviceGetMarginTemperature` |
| **Signature** | `nvmlReturn_t (nvmlDevice_t device, void *buf)` |
| **Struct Size** | 8 bytes |
| **Version** | 1 |
| **Version Word** | `0x01000008` |

**Struct Layout:**
```c
struct NvmlMarginTemperature {
    uint32_t version;           // = 0x01000008
    uint32_t margin_temp_celsius; // Thermal headroom in °C
};
```

**Observed value**: 54°C (thermal margin/headroom before throttling)

**Use case**: Shows how much thermal headroom remains before the GPU starts throttling. Complementary to standard NVML temperature readings.

---

### 2.3 nvmlDeviceGetDynamicPstatesInfo (No version check)

| Property | Value |
|----------|-------|
| **Symbol** | `nvmlDeviceGetDynamicPstatesInfo` |
| **Signature** | `nvmlReturn_t (nvmlDevice_t device, void *buf)` |
| **Struct Size** | 68 bytes |
| **Version** | None (no version check) |

**Struct Layout:**
```c
struct NvmlDynamicPstatesInfo {
    uint32_t flags;             // Always 0
    struct {
        uint32_t bIsPresent;    // 1 if slot is active
        uint32_t percentage;    // Utilization 0-100
    } slot[8];
};
```

**Slots observed**:
- Slot 0 (GPU): present=1, utilization varies
- Slot 1 (Frame Buffer): present=1, utilization varies
- Slot 2 (Video Engine): present=1, utilization varies
- Slots 3-7: not present

**Use case**: Per-engine utilization breakdown (more granular than `nvmlDeviceGetUtilizationRates`).

---

### 2.4 nvmlDeviceGetPowerMizerMode_v1 (No version check)

| Property | Value |
|----------|-------|
| **Symbol** | `nvmlDeviceGetPowerMizerMode_v1` |
| **Signature** | `nvmlReturn_t (nvmlDevice_t device, void *buf)` |
| **Struct Size** | 12 bytes |
| **Version** | None (no version check) |

**Struct Layout:**
```c
struct NvmlPowerMizerMode {
    uint32_t field0;    // Always 0
    uint32_t field1;    // Always 0
    uint32_t mode;      // PowerMizer mode value (observed: 7)
};
```

**Mode values**: 7 appears to be "adaptive" or "auto" mode.

---

### 2.5 VfOffset APIs (Standard NVML, undocumented ranges)

| Function | Range |
|----------|-------|
| `nvmlDeviceSetGpcClkVfOffset` | [-1000, +1000] mV offset |
| `nvmlDeviceSetMemClkVfOffset` | [-2000, +6000] mV offset |
| `nvmlDeviceGetGpcClkVfOffset` | Returns current GPU clock VF offset |
| `nvmlDeviceGetMemClkVfOffset` | Returns current memory clock VF offset |

These are the primary GPU overclocking interface on modern NVIDIA Linux drivers.

---

## 3. NVML Undocumented Functions - Unsupported on This Hardware <a name="nvml-unsupported"></a>

### 3.1 nvmlDeviceGetCoolerInfo
- **Correct version**: sz=16, ver=1, version_word=0x01000010
- **Status**: "Not Supported" — RTX 5090 Laptop fans are EC-controlled, not GPU-controlled
- **Would work on**: Desktop GPUs with direct fan control

### 3.2 nvmlDeviceGetWorkloadPowerProfile_v1
- **Status**: "Not Supported" — Not available on this laptop GPU
- **Likely available on**: Data center / professional GPUs

### 3.3 nvmlDeviceGetPowerSmoothing_v1
- **Status**: "Not Supported" — Laptop limitation
- **Likely available on**: Desktop GPUs with advanced power delivery

---

## 4. NVML Additional Discoveries <a name="nvml-additional"></a>

Through exhaustive symbol probing, these additional NVML facts were established:

| API | Value |
|-----|-------|
| `nvmlDeviceGetNumGpuCores` | 10496 CUDA cores |
| `nvmlDeviceGetArchitecture` | 10 (Blackwell) |
| `nvmlDeviceGetMemoryBusWidth` | 256 bits |
| `nvmlDeviceGetPcieLinkMaxSpeed` | PCIe Gen 5 |
| `nvmlDeviceGetMaxPcieLinkWidth` | x16 |
| `nvmlDeviceGetGspFirmwareVersion` | "590.48.01" |
| `nvmlDeviceGetThermalSettings` | No version check, always returns count=1 |
| `nvmlDeviceGetBoardPartNumber` | Board PN string |
| `nvmlDeviceGetGpuMaxPcieLinkGeneration` | 5 |

---

## 5. NvAPI Undocumented Functions - Working <a name="nvapi-working"></a>

### 5.1 AllClockFrequencies (0xDCB616C3) ⭐

| Property | Value |
|----------|-------|
| **ID** | 0xDCB616C3 |
| **Versions** | v1, v2, v3 all work |
| **Best Version** | v3 (supports clock type selection) |
| **Struct Size** | 264 bytes |

**Struct Layout (v3):**
```c
struct NvApiClockFrequencies_v3 {
    uint32_t version;           // = (264 & 0xFFFF) | (3 << 16)
    uint32_t clock_type;        // 0=CURRENT, 1=BASE, 2=BOOST, 3=TDP
    struct {
        uint32_t present;       // bit 0 = domain is present
        uint32_t freq_khz;      // Clock frequency in kHz
        uint8_t  padding[24];   // 24 bytes padding per domain
    } domain[8];                // 8 clock domains × 32 bytes
};
```

**Observed Values:**

| Clock Type | Domain 0 (GPU) | Domain 1 (Mem) | Domain 2 (SM?) |
|-----------|-----------------|-----------------|-----------------|
| CURRENT | 667-1110 MHz (varies) | 405 MHz (idle) | 937-1222 MHz (varies) |
| BASE | 1095 MHz | 14001 MHz | — |
| BOOST | 1597 MHz | 14001 MHz | — |
| TDP | 1597 MHz | 14001 MHz | — |

**Use case**: Real-time clock frequencies for all domains, plus base/boost/TDP reference clocks.

---

### 5.2 Voltage (0x465F9BCF) ⭐

| Property | Value |
|----------|-------|
| **ID** | 0x465F9BCF |
| **Version** | 1 |
| **Struct Size** | 76 bytes |

**Struct Layout:**
```c
struct NvApiVoltage {
    uint32_t version;       // = (76 & 0xFFFF) | (1 << 16)
    uint32_t flags;         // 0
    uint32_t padding_1[8];  // 32 bytes padding
    uint32_t value_uv;      // Voltage in microvolts (offset 0x28)
    uint32_t padding_2[8];  // 32 bytes padding
};
```

**Observed**: 705000-740000 µV (0.705-0.740V), fluctuates with load.

**LACT confirmed**: LACT uses identical struct layout. Our reverse engineering matches exactly.

---

### 5.3 Thermals (0x65FE3AAD)

| Property | Value |
|----------|-------|
| **ID** | 0x65FE3AAD |
| **Version** | 2 |
| **Struct Size** | 176 bytes |

**Struct Layout:**
```c
struct NvApiThermals {
    uint32_t version;       // = (176 & 0xFFFF) | (2 << 16)
    int32_t  mask;          // Bitmask of requested sensors
    int32_t  values[40];    // Raw thermal values (divide by 256 for °C)
};
```

**Key indices** (divide value by 256):
- Index 9: GPU Hotspot temperature
- Index 15: VRAM temperature

**LACT uses this same function** with identical struct.

---

### 5.4 ClientPowerTopologyGetInfo (0x60DED2ED)

| Property | Value |
|----------|-------|
| **ID** | 0x60DED2ED |
| **Version** | 1 |
| **Struct Size** | 72 bytes |

**Struct Layout (partial):**
```c
struct NvApiClientPowerTopology {
    uint32_t version;
    uint32_t entry_count;   // Number of power domain entries
    struct {
        uint32_t domain;    // Power domain identifier
        uint32_t type;      // Domain type
        uint32_t flags;
        uint32_t power_w;   // Power limit/budget in watts
    } entries[];
};
```

**Observed data**: Entry with value 83 at offset 0x24, likely the 83W TDP of the RTX 5090 Laptop GPU.

---

### 5.5 PerfPoliciesGetStatus (0x3D358A0C)

| Property | Value |
|----------|-------|
| **ID** | 0x3D358A0C |
| **Version** | 1 |
| **Struct Size** | 1360 bytes |

Contains performance policy status with:
- Timestamps and counters
- Performance limiter bitmask (observed: 0x0F, indicating multiple active limiters)

---

### 5.6 Unknown_0x0D258BB5 (Possibly Clock Limits)

| Property | Value |
|----------|-------|
| **ID** | 0x0D258BB5 |
| **Versions** | v1 (88 bytes) and v2 (104 bytes) |

**Struct Layout:**
```c
struct NvApiUnknown_0D258BB5_v1 {
    uint32_t version;       // [0]
    uint32_t field1;        // [1] = 257 (0x101) — flags/type?
    uint32_t field2;        // [2] = 0
    uint32_t count;         // [3] = 1 — number of entries?
    uint32_t min_value;     // [4] = 19200 — 19200 kHz = 19.2 MHz (reference clock?)
    uint32_t max_value;     // [5] = 22272 — 22272 kHz = 22.272 MHz
    uint32_t current_value; // [6] = 22272 — matches max
    uint32_t reserved[15];  // [7-21] = all zeros
};
```

V2 (104 bytes) adds: `[7]=1` instead of 0, plus 16 bytes of zeros at the end.

**Interpretation**: The values 19200/22272 are suspiciously close to common reference clock frequencies. 19.2 MHz is a standard base oscillator. This may be a reference clock or PLL configuration query.

---

### 5.7 GetCurrentPstate (0x927DA4F6) - Special

| Property | Value |
|----------|-------|
| **ID** | 0x927DA4F6 |
| **No version check** | Works with any version/size |
| **Min struct** | 8 bytes |

Returns current P-state index. Field at offset 4 = 0 means P0 (highest performance state).

---

### 5.8 PowerPoliciesGetInfo (0x34574232) & GetStatus (0x70916171)

Both work but return all zeros on Blackwell. The traditional NvAPI power policy mechanism appears to not be used on this architecture.

---

## 6. NvAPI Functions - Unavailable on Blackwell <a name="nvapi-unavailable"></a>

These functions have valid function pointers (they exist in the library) but return version mismatch/data-not-found for **ALL** tested sizes (8 to 32768 bytes) and versions (1 to 5):

| Function | ID | Status |
|----------|----|--------|
| **GetClockBoostLock** | 0xE440B867 | Available, no working version (all sizes 8-32768, ver 1-5) |
| **GetClockBoostTable** | 0x23F1B133 | Available, no working version |
| **GetVFPCurve** | 0x21537AD4 | Available, no working version |
| **GetClockBoostRanges** | 0x64B43A6A | Available, no working version |
| **GetPstates20** | 0x6FF81213 | Available, DATA_NOT_FOUND |
| **GetPstatesInfoEx** | 0x843C0256 | Available, no working version |
| **FanCooler functions** | Various | NOT AVAILABLE (laptop, EC-controlled fans) |

**Conclusion**: Blackwell does NOT expose the Boost 4.0 clock table / VF curve via NvAPI. The Boost 4.0 framework (GetClockBoostLock/Table/VFPCurve) was introduced around Pascal/Turing and may have been replaced or disabled on Blackwell, especially for laptop variants. The function entry points exist as stubs but the underlying GPU firmware doesn't support these queries.

For clock offsetting on Blackwell, use **NVML VfOffset APIs** (`nvmlDeviceSetGpcClkVfOffset` / `nvmlDeviceSetMemClkVfOffset`).

---

## 7. LACT Cross-Reference <a name="lact-comparison"></a>

After analyzing LACT's full NVIDIA codebase:

### What LACT Uses
1. **Standard nvml_wrapper crate** — All documented NVML functions
2. **NvAPI 0x65FE3AAD** (Thermals v2) — GPU Hotspot + VRAM temps
3. **NvAPI 0x465F9BCF** (Voltage v1) — Real-time GPU voltage
4. **Direct kernel RM ioctls** (`NV_ESC_RM_CONTROL`) for:
   - RAM type, vendor, bus width
   - SM count, ROP info, L2 cache size
   - Uses NVIDIA kernel headers (cl2080.h, ctrl2080fb.h, ctrl2080gr.h)

### What LACT Does NOT Use
- Any undocumented NVML functions (no CoolerInfo, PerformanceModes, MarginTemperature, etc.)
- NvAPI ClockBoostLock/Table/VFPCurve
- NvAPI PerfPolicies, PowerPolicies, ClientPowerTopology
- NvAPI AllClockFrequencies (uses standard NVML clock queries instead)

### LACT Struct Validation
Our reverse-engineered `NvApiVoltage` struct matches LACT's implementation exactly:
- LACT: `version + flags + padding_1[8] + value_uv + padding_2[8]` = 76 bytes
- Our finding: Voltage at offset 0x28 (40 bytes = 4 + 4 + 32), value in µV ✓

---

## 8. Disassembly Insights <a name="disassembly"></a>

### NVML Function Dispatch Pattern
All undocumented NVML functions follow the same pattern:

```
function_entry:
    test   rdi, rdi          ; check device handle != NULL
    je     return_error
    mov    rax, [rdi+0x10]   ; get device internal pointer
    test   rax, rax
    je     return_error
    mov    rcx, [rax+0x1a688] ; get vtable pointer (offset varies by driver version)
    mov    rcx, [rcx+0x118]   ; second vtable indirection
    jmp    [rcx+0x88]         ; jump to actual implementation (offset varies per function)
```

Key insight: All version checking happens inside the vtable target function, which is in driver-internal code not visible from userspace disassembly. This is why brute-force version scanning was necessary.

### NvAPI Function Dispatch
NvAPI functions route through `nvapi_QueryInterface` → internal dispatch table → function implementation. Each function validates the version word at the start: `(size & 0xFFFF) | (ver << 16)`.

---

## 9. UCC Integration Recommendations <a name="ucc-integration"></a>

### High-Value APIs for UCC

| Priority | API | Why |
|----------|-----|-----|
| **P0** | NVML VfOffset (GpcClk/MemClk) | Primary OC interface — already implemented ✓ |
| **P1** | NvAPI AllClockFrequencies v3 | Real-time + base/boost/TDP clocks, 3 domains |
| **P1** | NvAPI Voltage (0x465F9BCF) | Real-time GPU voltage monitoring |
| **P2** | NVML PerformanceModes | Full P-state table with min/max/editability |
| **P2** | NVML MarginTemperature | Thermal headroom monitoring |
| **P2** | NvAPI Thermals (0x65FE3AAD) | GPU Hotspot + VRAM temperature |
| **P3** | NVML DynamicPstatesInfo | Per-engine utilization breakdown |
| **P3** | NvAPI ClientPowerTopology | Power budget/topology information |
| **P3** | NVML PowerMizerMode | Current PowerMizer setting |

### Implementation Notes
- **AllClockFrequencies**: Must `dlopen("libnvidia-api.so.1")`, resolve `nvapi_QueryInterface`, initialize NvAPI, enumerate GPUs, then call. UCC already has NvmlWrapper which can be extended.
- **VfOffset**: Already implemented in UCC's NvmlWrapper.
- **MarginTemperature/PerformanceModes**: Simple NVML calls with versioned struct, can be added alongside existing NVML wrapper.
- **Voltage/Thermals**: Needs NvAPI handle infrastructure (separate from NVML).

### What NOT to Pursue
- GetClockBoostLock/Table/VFPCurve — don't work on Blackwell
- GetPstates20 — DATA_NOT_FOUND on Blackwell
- CoolerInfo — laptop-only limitation (needs desktop GPU to test)
- PowerPoliciesGetInfo/GetStatus — returns all zeros on Blackwell

---

## 10. Version Encoding Reference <a name="version-encoding"></a>

### NVML Version Words
```
0x01000804 = PerformanceModes     (sz=2052, ver=1)
0x01000008 = MarginTemperature    (sz=8,    ver=1)
0x01000010 = CoolerInfo           (sz=16,   ver=1) [not supported on laptop]
```
Formula: `(ver << 24) | (size & 0x00FFFFFF)`

### NvAPI Version Words
```
0x00030108 = AllClockFrequencies  (sz=264,  ver=3)
0x0001004C = Voltage              (sz=76,   ver=1)
0x000200B0 = Thermals             (sz=176,  ver=2)
0x00010048 = ClientPowerTopology  (sz=72,   ver=1)
0x00010550 = PerfPoliciesStatus   (sz=1360, ver=1)
0x00010058 = Unknown_0D258BB5     (sz=88,   ver=1)
0x00020068 = Unknown_0D258BB5     (sz=104,  ver=2)
```
Formula: `(ver << 16) | (size & 0xFFFF)`

---

## Appendix: Probe Programs

All probe programs are saved in `/tmp/`:
- `deep_nvml_probe.c` — Initial comprehensive NVML probe
- `nvml_scan2.c` — Safe version scanner (cracked PerformanceModes & MarginTemperature)
- `nvml_detail.c` — Detailed data dump for cracked functions
- `deep_nvapi.c` — NvAPI function discovery and initial probing
- `deep_fields.c` — Comprehensive field mapping for all working functions
- `clockboost_scan.c` — Extended brute-force scan (sizes to 32768, versions 1-5)
- `final_detail.c` — Final data collection for unknowns
