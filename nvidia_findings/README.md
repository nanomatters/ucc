# NVIDIA Findings - Index

This directory contains all research files, probes, and outputs from the comprehensive NVIDIA GPU undocumented API investigation.

## Main Findings Document
- **nvidia_undocumented_api_findings.md** - Comprehensive findings document with all discovered APIs, struct layouts, and arch limitations

## Probe Programs & Source Code

### NVML Probes
- **deep_nvml_probe.c** / **deep_nvml_probe** - Initial comprehensive NVML function scanner
- **nvml_fine_scan.c** / **nvml_fine_scan** - Fine-grained NVML version/size scanner
- **nvml_scan2.c** / **nvml_scan2** - Secondary NVML safe version scanner
- **nvml_detail.c** / **nvml_detail** - NVML struct field dumper
- **nvml-symbols.txt** - Extracted NVML library symbols

### NvAPI Probes  
- **probe_nvapi.c** / **probe_nvapi** - Initial NvAPI symbol enumeration
- **probe_nvapi_data.c** / **probe_nvapi_data** - NvAPI raw data retriever
- **deep_nvapi.c** / **deep_nvapi** - Comprehensive NvAPI function discovery with struct layout detection
- **nvapi-symbols.txt** - Extracted NvAPI library symbols

### Exhaustive Scanners
- **clockboost_scan.c** / **clockboost_scan** - Comprehensive ClockBoost API family brute-force scanner (10,000+ combinations)
- **deep_fields.c** / **deep_fields** - Mystery function field layout dumper

### Utility
- **final_detail.c** / **final_detail** - Final detailed field dump for remaining unknowns

## Output Files

### NVML Outputs
- **deep_nvml_output.txt** - Initial NVML discovery output (153KB)
- **nvml_fine_scan_output.txt** - Fine-scan results

### NvAPI & Combined Outputs
- **deep_nvapi_output.txt** - Comprehensive NvAPI function discovery with parameters (68KB)
- **deep_fields_output.txt** - Mystery function field dumps
- **nvml_scan2_output.txt** - Secondary NVML scan results
- **nvml_detail_output.txt** - NVML struct field details

## Organization

**Source Files (.c)**: Probe programs written in C with NVML/NvAPI FFI bindings
**Compiled Binaries**: Ready-to-run executables (compiled with GCC -O2)
**Output Files (.txt)**: Raw output from probe execution
**Symbol Lists**: Extracted function symbols from libraries

## Key Discoveries

### Working NVML Functions (Undocumented)
- PerformanceModes (2052B, v1)
- MarginTemperature (8B, v1) 
- DynamicPstatesInfo (68B, v1)
- PowerMizerMode_v1 (12B, v1)

### Working NvAPI Functions (Undocumented)
- AllClockFrequencies (0xDCB616C3) - 264B, v1-3, supports ClockType parameter
- Voltage (0x465F9BCF) - 76B, v1 - **struct validated against LACT production code**
- ClientPowerTopology (0x60DED2ED) - 72B, v1
- PerfPoliciesGetStatus (0x3D358A0C) - 1360B, v1
- Thermals (0x65FE3AAD) - 176B, v2 - **used by LACT**
- Unknown 0x0D258BB5 - Reference clock data (88B v1, 104B v2 with extension)

### Confirmed Unavailable (Blackwell Architecture)
- ClockBoost family (GetClockBoostLock, GetClockBoostTable, GetVFPCurve, etc.)
- GetPstates20 (legacy Maxwell/Kepler API)
- CoolerInfo (laptop EC-controlled fans)

## 2026-03 Update: Stability Telemetry Findings

Additional investigation (read-only analysis + host symbol verification) found that
`libnvidia-ml.so.1` exports several reliability and limiter APIs that were not yet
integrated into UCC stability logic.

### Host-verified NVML exports (candidate stability inputs)
- `nvmlDeviceGetViolationStatus`
- `nvmlDeviceGetCurrentClocksEventReasons`
- `nvmlDeviceGetCurrentClocksThrottleReasons`
- `nvmlDeviceGetTotalEnergyConsumption`
- `nvmlDeviceGetFieldValues`
- `nvmlDeviceGetRetiredPages`, `nvmlDeviceGetRetiredPages_v2`
- `nvmlDeviceGetRemappedRows`
- `nvmlDeviceGetRepairStatus`
- `nvmlDeviceGetDetailedEccErrors`
- `nvmlDeviceGetMemoryErrorCounter`
- `nvmlDeviceGetSramEccErrorStatus`

These were confirmed as exported symbols via `readelf -Ws` on this host. Exported
does not automatically mean struct layouts/semantics are fully mapped in this repo.

### Already reverse-validated in this folder (high confidence)
- NVML: PerformanceModes, MarginTemperature, DynamicPstatesInfo, PowerMizerMode_v1
- NvAPI: AllClockFrequencies (0xDCB616C3), Voltage (0x465F9BCF),
	ClientPowerTopologyGetInfo (0x60DED2ED), PerfPoliciesGetStatus (0x3D358A0C)

### Consistency notes and caveats
- There are conflicting IDs across files for ClockBoost/VF table family:
	- `GetClockBoostTable`: `0x23F1B133` vs `0x507B4B59`
	- `GetVFPCurve`: `0x21537AD4` vs `0x7F5F90A7`
- Thermal function consistency is mixed across outputs:
	- Some notes describe Thermals (0x65FE3AAD) as usable
	- `deep_nvapi_output.txt` shows `NVAPI_DATA_NOT_FOUND` in tested layouts

Practical recommendation: keep capability probing and fallbacks at runtime rather
than assuming uniform behavior across driver/GPU generations.

## Hardware / Environment

- **Target**: RTX 5090 Laptop
- **Architecture**: Blackwell (arch=10)
- **Driver**: NVIDIA 590.48.01
- **NVML Library**: libnvidia-ml.so.590.48.01 (2.2MB, 405 symbols)
- **NvAPI Library**: libnvidia-api.so.1 (724KB, nvapi_QueryInterface export)
- **OS**: Fedora 45
- **Compiler**: GCC 16.0.1

## Version Encoding

**NVML**: `(struct_size & 0x00FFFFFF) | (version_number << 24)`
**NvAPI**: `(struct_size & 0xFFFF) | (version_number << 16)`

## Related Project

**LACT**: Open-source GPU control daemon (Rust)
- Uses standard NVML via nvml_wrapper crate
- Uses only 2 undocumented NvAPI: thermals (0x65FE3AAD) and voltage (0x465F9BCF)
- Uses kernel RM ioctls for hardware info via /dev/nvidiactl
- nvapi.rs struct definitions validate our reverse-engineered layouts exactly
