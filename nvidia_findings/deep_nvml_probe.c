/*
 * Deep NVML Undocumented API Probe
 * Systematically crack struct versions and map field layouts
 * for all undocumented NVML functions discovered previously.
 *
 * Build: gcc -O0 -g -o /tmp/deep_nvml_probe /tmp/deep_nvml_probe.c -ldl
 * Run:   sudo /tmp/deep_nvml_probe 2>&1
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <stdint.h>

typedef void *nvmlDevice_t;
typedef int nvmlReturn_t;

/* ─── Documented functions we need ─── */
typedef nvmlReturn_t (*fn_nvmlInit_v2)(void);
typedef nvmlReturn_t (*fn_nvmlShutdown)(void);
typedef nvmlReturn_t (*fn_nvmlDeviceGetHandleByIndex_v2)(unsigned int, nvmlDevice_t *);
typedef nvmlReturn_t (*fn_nvmlDeviceGetName)(nvmlDevice_t, char *, unsigned int);
typedef const char * (*fn_nvmlErrorString)(nvmlReturn_t);

/* ─── Generic versioned struct call: fn(device, &struct) ─── */
typedef nvmlReturn_t (*fn_dev_struct)(nvmlDevice_t, void *);

/* ─── Helper to make versioned NVML version field ─── */
/* NVML version = (structSize & 0xFFFFFF) | (versionNum << 24) */
static inline uint32_t make_version(uint32_t struct_size, uint32_t ver_num) {
    return (struct_size & 0x00FFFFFF) | (ver_num << 24);
}

static void *lib = NULL;
static nvmlDevice_t device = NULL;
static fn_nvmlErrorString errStr = NULL;

static void hexdump(const void *data, size_t size, size_t max_bytes) {
    const uint8_t *p = (const uint8_t *)data;
    size_t n = size < max_bytes ? size : max_bytes;
    for (size_t i = 0; i < n; i++) {
        if (i % 32 == 0) printf("    %04zx: ", i);
        printf("%02x", p[i]);
        if (i % 4 == 3) printf(" ");
        if (i % 32 == 31 || i == n - 1) printf("\n");
    }
}

static void hexdump_nonzero(const void *data, size_t size) {
    const uint8_t *p = (const uint8_t *)data;
    /* Find last non-zero byte */
    size_t last = 0;
    for (size_t i = 0; i < size; i++) {
        if (p[i] != 0) last = i;
    }
    size_t show = (last + 32) & ~31ULL;
    if (show > size) show = size;
    if (show < 64) show = 64; /* Always show at least 64 bytes */
    hexdump(data, show, show);
}

/* Try different struct sizes and version numbers for a function.
 * Returns 1 if any version worked. */
static int brute_force_version(const char *func_name, fn_dev_struct fn,
                               const uint32_t *sizes, int n_sizes,
                               int max_ver) {
    printf("\n══════════════════════════════════════════════\n");
    printf("  Brute-forcing: %s\n", func_name);
    printf("══════════════════════════════════════════════\n");

    uint8_t buf[65536];
    int found = 0;

    for (int si = 0; si < n_sizes; si++) {
        for (int v = 1; v <= max_ver; v++) {
            uint32_t sz = sizes[si];
            uint32_t ver = make_version(sz, v);

            memset(buf, 0, sizeof(buf));
            *(uint32_t *)buf = ver;

            nvmlReturn_t ret = fn(device, buf);

            if (ret == 0) { /* SUCCESS */
                printf("  ✓ SUCCESS: size=%u, ver=%u (version_field=0x%08x)\n", sz, v, ver);
                printf("  Raw data (first non-zero region):\n");
                hexdump_nonzero(buf, sz);
                found = 1;
                /* Don't break - try other versions too to see if multiple work */
            } else if (ret != -9 && ret != 15) {
                /* -9 = INCOMPATIBLE_STRUCT_VERSION, 15 = NVML_ERROR_FUNCTION_NOT_FOUND
                 * Other errors mean the version was accepted but something else happened */
                printf("  ✗ size=%u, ver=%u => ret=%d (%s)\n",
                       sz, v, ret, errStr ? errStr(ret) : "?");
            }
        }
    }

    if (!found) {
        printf("  No working version found in tested range.\n");
    }
    return found;
}

/* ─── Parse struct fields for known-good versions ─── */

static void probe_cooler_info(fn_dev_struct fn) {
    /* Based on NV open-source hints, coolerInfo typically contains:
     * uint32_t version
     * uint32_t count
     * struct { uint32_t coolerType; uint32_t controller; ... } entries[]
     */
    static const uint32_t sizes[] = {
        /* Try common NVML struct sizes */
        32, 36, 40, 44, 48, 52, 56, 60, 64,
        68, 72, 76, 80, 84, 88, 92, 96, 100,
        104, 108, 112, 116, 120, 124, 128,
        136, 144, 152, 160, 168, 176, 184, 192,
        200, 208, 216, 224, 232, 240, 248, 256,
        264, 288, 320, 384, 448, 512, 640, 768,
        1024, 1536, 2048, 4096
    };
    brute_force_version("nvmlDeviceGetCoolerInfo", fn,
                        sizes, sizeof(sizes)/sizeof(sizes[0]), 3);
}

static void probe_performance_modes(fn_dev_struct fn) {
    static const uint32_t sizes[] = {
        32, 36, 40, 44, 48, 52, 56, 60, 64,
        68, 72, 76, 80, 84, 88, 92, 96, 100,
        104, 108, 112, 116, 120, 124, 128,
        136, 144, 152, 160, 168, 176, 184, 192,
        200, 208, 216, 224, 232, 240, 248, 256,
        264, 288, 320, 384, 448, 512, 640, 768,
        1024, 1536, 2048, 4096, 8192
    };
    brute_force_version("nvmlDeviceGetPerformanceModes", fn,
                        sizes, sizeof(sizes)/sizeof(sizes[0]), 3);
}

static void probe_margin_temperature(fn_dev_struct fn) {
    static const uint32_t sizes[] = {
        16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60, 64,
        68, 72, 76, 80, 84, 88, 92, 96, 100,
        104, 108, 112, 116, 120, 124, 128,
        136, 144, 152, 160, 168, 176, 184, 192,
        200, 208, 216, 224, 232, 240, 248, 256,
        264, 288, 320, 384, 448, 512
    };
    brute_force_version("nvmlDeviceGetMarginTemperature", fn,
                        sizes, sizeof(sizes)/sizeof(sizes[0]), 3);
}

static void probe_workload_power_profiles_info(fn_dev_struct fn) {
    static const uint32_t sizes[] = {
        16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60, 64,
        68, 72, 76, 80, 84, 88, 92, 96, 100,
        104, 108, 112, 116, 120, 124, 128,
        136, 144, 152, 160, 168, 176, 184, 192,
        200, 208, 216, 224, 232, 240, 248, 256,
        264, 288, 320, 384, 448, 512, 640, 768,
        1024, 1536, 2048, 4096
    };
    brute_force_version("nvmlDeviceWorkloadPowerProfileGetProfilesInfo", fn,
                        sizes, sizeof(sizes)/sizeof(sizes[0]), 3);
}

static void probe_workload_power_current(fn_dev_struct fn) {
    static const uint32_t sizes[] = {
        8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60, 64,
        68, 72, 76, 80, 84, 88, 92, 96, 100,
        104, 108, 112, 116, 120, 124, 128,
        136, 144, 152, 160, 168, 176, 184, 192,
        200, 208, 216, 224, 232, 240, 248, 256
    };
    brute_force_version("nvmlDeviceWorkloadPowerProfileGetCurrentProfiles", fn,
                        sizes, sizeof(sizes)/sizeof(sizes[0]), 3);
}

static void probe_power_smoothing(fn_dev_struct fn) {
    static const uint32_t sizes[] = {
        8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60, 64,
        68, 72, 76, 80, 84, 88, 92, 96, 100,
        104, 108, 112, 116, 120, 124, 128,
        136, 144, 152, 160, 168, 176, 184, 192,
        200, 208, 216, 224, 232, 240, 248, 256,
        264, 288, 320, 384, 448, 512, 640, 768, 1024
    };
    brute_force_version("nvmlDevicePowerSmoothingSetState", fn,
                        sizes, sizeof(sizes)/sizeof(sizes[0]), 3);
}

/* ─── Deep probe for already-working functions to map their fields ─── */

static void deep_probe_dynamic_pstates(fn_dev_struct fn) {
    printf("\n══════════════════════════════════════════════\n");
    printf("  Deep field mapping: nvmlDeviceGetDynamicPstatesInfo\n");
    printf("══════════════════════════════════════════════\n");

    /* This one works without a version header, just pass a buffer */
    uint8_t buf[512];
    memset(buf, 0, sizeof(buf));

    nvmlReturn_t ret = fn(device, buf);
    if (ret == 0) {
        printf("  SUCCESS! Full struct dump:\n");
        hexdump(buf, 512, 512);

        /* Known layout based on NVML headers:
         * uint32_t flags
         * struct { uint32_t bIsPresent; uint32_t percentage; } utilization[8]
         *   [0]=GPU, [1]=FB, [2]=VID, [3]=BUS
         */
        uint32_t *u32 = (uint32_t *)buf;
        printf("\n  Parsed fields:\n");
        printf("    flags: 0x%08x\n", u32[0]);
        const char *names[] = {"GPU", "FB/MEM", "VIDEO", "BUS", "SLOT4", "SLOT5", "SLOT6", "SLOT7"};
        for (int i = 0; i < 8; i++) {
            uint32_t present = u32[1 + i*2];
            uint32_t pct = u32[2 + i*2];
            if (present || pct)
                printf("    [%d] %s: present=%u, util=%u%%\n", i, names[i], present, pct);
        }
    } else {
        printf("  Failed: ret=%d (%s)\n", ret, errStr ? errStr(ret) : "?");
    }
}

static void deep_probe_gpc_clk_vf_offset(void *lib_handle) {
    printf("\n══════════════════════════════════════════════\n");
    printf("  Deep field mapping: GpcClk/MemClk VfOffset APIs\n");
    printf("══════════════════════════════════════════════\n");

    typedef nvmlReturn_t (*fn_get_offset)(nvmlDevice_t, int *);
    typedef nvmlReturn_t (*fn_get_minmax)(nvmlDevice_t, int *, int *);

    fn_get_offset getGpc = (fn_get_offset)dlsym(lib_handle, "nvmlDeviceGetGpcClkVfOffset");
    fn_get_offset getMem = (fn_get_offset)dlsym(lib_handle, "nvmlDeviceGetMemClkVfOffset");
    fn_get_minmax getGpcMM = (fn_get_minmax)dlsym(lib_handle, "nvmlDeviceGetGpcClkMinMaxVfOffset");
    fn_get_minmax getMemMM = (fn_get_minmax)dlsym(lib_handle, "nvmlDeviceGetMemClkMinMaxVfOffset");

    int offset = 0, min_off = 0, max_off = 0;

    if (getGpc) {
        nvmlReturn_t ret = getGpc(device, &offset);
        printf("  GpcClkVfOffset: %d MHz (ret=%d)\n", offset, ret);
    }
    if (getGpcMM) {
        nvmlReturn_t ret = getGpcMM(device, &min_off, &max_off);
        printf("  GpcClkMinMaxVfOffset: %d to %d MHz (ret=%d)\n", min_off, max_off, ret);
    }
    if (getMem) {
        nvmlReturn_t ret = getMem(device, &offset);
        printf("  MemClkVfOffset: %d MHz (ret=%d)\n", offset, ret);
    }
    if (getMemMM) {
        nvmlReturn_t ret = getMemMM(device, &min_off, &max_off);
        printf("  MemClkMinMaxVfOffset: %d to %d MHz (ret=%d)\n", min_off, max_off, ret);
    }
}

static void deep_probe_power_mizer(void *lib_handle) {
    printf("\n══════════════════════════════════════════════\n");
    printf("  Deep field mapping: PowerMizerMode_v1\n");
    printf("══════════════════════════════════════════════\n");

    fn_dev_struct getMode = (fn_dev_struct)dlsym(lib_handle, "nvmlDeviceGetPowerMizerMode_v1");
    if (!getMode) { printf("  Not found\n"); return; }

    /* Try buffer of different sizes to see how much data it writes */
    for (int sz = 4; sz <= 64; sz += 4) {
        uint8_t buf[64];
        memset(buf, 0xAA, sizeof(buf));  /* Sentinel pattern */

        nvmlReturn_t ret = getMode(device, buf);
        if (ret == 0) {
            /* Find how many bytes were written (changed from 0xAA) */
            int last_written = 0;
            for (int i = 0; i < sz; i++) {
                if (buf[i] != 0xAA) last_written = i;
            }
            printf("  size=%d: SUCCESS, last_written_byte=%d\n", sz, last_written);
            if (sz == 4) {
                printf("    mode value = %u (as uint32)\n", *(uint32_t*)buf);
            }
            if (sz >= 32) {
                printf("    Raw:\n");
                hexdump(buf, sz, sz);
                break;
            }
        } else {
            printf("  size=%d: ret=%d\n", sz, ret);
        }
    }
}

static void deep_probe_fan_apis(void *lib_handle) {
    printf("\n══════════════════════════════════════════════\n");
    printf("  Deep field mapping: Fan control APIs\n");
    printf("══════════════════════════════════════════════\n");

    typedef nvmlReturn_t (*fn_get)(nvmlDevice_t, unsigned int *);
    typedef nvmlReturn_t (*fn_get2)(nvmlDevice_t, unsigned int, unsigned int *);
    typedef nvmlReturn_t (*fn_minmax)(nvmlDevice_t, unsigned int *, unsigned int *);

    fn_get getSpeed = (fn_get)dlsym(lib_handle, "nvmlDeviceGetFanSpeed");
    fn_get2 getSpeed2 = (fn_get2)dlsym(lib_handle, "nvmlDeviceGetFanSpeed_v2");
    fn_get2 getRPM = (fn_get2)dlsym(lib_handle, "nvmlDeviceGetFanSpeedRPM");
    fn_get2 getTarget = (fn_get2)dlsym(lib_handle, "nvmlDeviceGetTargetFanSpeed");
    fn_minmax getMinMax = (fn_minmax)dlsym(lib_handle, "nvmlDeviceGetMinMaxFanSpeed");

    unsigned int val = 0, min_v = 0, max_v = 0;

    if (getSpeed) {
        nvmlReturn_t ret = getSpeed(device, &val);
        printf("  FanSpeed: %u%% (ret=%d)\n", val, ret);
    }
    for (unsigned int fan = 0; fan < 4; fan++) {
        if (getSpeed2) {
            nvmlReturn_t ret = getSpeed2(device, fan, &val);
            if (ret == 0 || fan == 0)
                printf("  FanSpeed_v2[%u]: %u%% (ret=%d)\n", fan, val, ret);
            if (ret != 0) break;
        }
        if (getRPM) {
            nvmlReturn_t ret = getRPM(device, fan, &val);
            if (ret == 0 || fan == 0)
                printf("  FanSpeedRPM[%u]: %u (ret=%d)\n", fan, val, ret);
            if (ret != 0) break;
        }
        if (getTarget) {
            nvmlReturn_t ret = getTarget(device, fan, &val);
            if (ret == 0 || fan == 0)
                printf("  TargetFanSpeed[%u]: %u%% (ret=%d)\n", fan, val, ret);
            if (ret != 0) break;
        }
    }
    if (getMinMax) {
        nvmlReturn_t ret = getMinMax(device, &min_v, &max_v);
        printf("  MinMaxFanSpeed: %u - %u%% (ret=%d)\n", min_v, max_v, ret);
    }
}

/* ─── Power Smoothing deep probe ─── */
static void deep_probe_power_smoothing(void *lib_handle) {
    printf("\n══════════════════════════════════════════════\n");
    printf("  Deep probe: PowerSmoothing APIs\n");
    printf("══════════════════════════════════════════════\n");

    const char *names[] = {
        "nvmlDevicePowerSmoothingActivatePresetProfile",
        "nvmlDevicePowerSmoothingUpdatePresetProfileParam",
        "nvmlDevicePowerSmoothingSetState",
    };

    for (int ni = 0; ni < 3; ni++) {
        fn_dev_struct fn = (fn_dev_struct)dlsym(lib_handle, names[ni]);
        if (!fn) { printf("  %s: not found\n", names[ni]); continue; }

        static const uint32_t sizes[] = {
            8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60, 64,
            68, 72, 76, 80, 84, 88, 92, 96, 100,
            104, 108, 112, 116, 120, 124, 128,
            136, 144, 152, 160, 168, 176, 184, 192,
            200, 208, 216, 224, 232, 240, 248, 256,
            264, 288, 320, 384, 448, 512
        };
        brute_force_version(names[ni], fn, sizes, sizeof(sizes)/sizeof(sizes[0]), 3);
    }
}

/* ─── WorkloadPowerProfile deep probe ─── */
static void deep_probe_workload_power(void *lib_handle) {
    printf("\n══════════════════════════════════════════════\n");
    printf("  Deep probe: WorkloadPowerProfile APIs\n");
    printf("══════════════════════════════════════════════\n");

    const char *names[] = {
        "nvmlDeviceWorkloadPowerProfileGetProfilesInfo",
        "nvmlDeviceWorkloadPowerProfileGetCurrentProfiles",
        "nvmlDeviceWorkloadPowerProfileSetRequestedProfiles",
        "nvmlDeviceWorkloadPowerProfileClearRequestedProfiles",
        "nvmlDeviceWorkloadPowerProfileUpdateProfiles_v1",
    };

    for (int ni = 0; ni < 5; ni++) {
        fn_dev_struct fn = (fn_dev_struct)dlsym(lib_handle, names[ni]);
        if (!fn) { printf("  %s: not found\n", names[ni]); continue; }

        static const uint32_t sizes[] = {
            8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60, 64,
            68, 72, 76, 80, 84, 88, 92, 96, 100,
            104, 108, 112, 116, 120, 124, 128,
            136, 144, 152, 160, 168, 176, 184, 192,
            200, 208, 216, 224, 232, 240, 248, 256,
            264, 288, 320, 384, 448, 512, 640, 768, 1024, 2048, 4096
        };
        brute_force_version(names[ni], fn, sizes, sizeof(sizes)/sizeof(sizes[0]), 3);
    }
}

/* ─── Additional undocumented functions we haven't probed yet ─── */
static void probe_all_undocumented_nvml(void *lib_handle) {
    printf("\n══════════════════════════════════════════════\n");
    printf("  Probing ALL remaining undocumented NVML functions\n");
    printf("══════════════════════════════════════════════\n");

    /* List of function names to try with simple (device, &val) or (device, &struct) signatures */
    const char *simple_functions[] = {
        "nvmlDeviceGetAdaptiveClockInfoStatus",
        "nvmlDeviceGetGspFirmwareVersion",
        "nvmlDeviceGetGspFirmwareMode",
        "nvmlDeviceGetArchitecture",
        "nvmlDeviceGetPcieThroughput",
        "nvmlDeviceGetMemoryBusWidth",
        "nvmlDeviceGetMaxPcieLinkGeneration",
        "nvmlDeviceGetMaxPcieLinkWidth",
        "nvmlDeviceGetCurrPcieLinkGeneration",
        "nvmlDeviceGetCurrPcieLinkWidth",
        "nvmlDeviceGetTotalEnergyConsumption",
        "nvmlDeviceGetPowerSource",
        "nvmlDeviceGetThermalSettings",
        "nvmlDeviceGetNumGpuCores",
        "nvmlDeviceGetBusType",
        "nvmlDeviceGetDynamicBoostLimit",
        NULL
    };

    for (int i = 0; simple_functions[i]; i++) {
        void *fn = dlsym(lib_handle, simple_functions[i]);
        if (fn) printf("  [FOUND] %s\n", simple_functions[i]);
    }

    /* Try GetThermalSettings with versioned struct */
    fn_dev_struct getThermal = (fn_dev_struct)dlsym(lib_handle, "nvmlDeviceGetThermalSettings");
    if (getThermal) {
        printf("\n  Brute-forcing nvmlDeviceGetThermalSettings:\n");
        static const uint32_t sizes[] = {
            32, 40, 48, 56, 60, 64, 72, 80, 88, 96, 104, 112, 120, 128,
            136, 144, 152, 160, 168, 176, 184, 192, 200, 256, 512
        };
        brute_force_version("nvmlDeviceGetThermalSettings", getThermal,
                            sizes, sizeof(sizes)/sizeof(sizes[0]), 3);
    }

    /* Try GetDynamicBoostLimit */
    fn_dev_struct getBoost = (fn_dev_struct)dlsym(lib_handle, "nvmlDeviceGetDynamicBoostLimit");
    if (getBoost) {
        printf("\n  Probing nvmlDeviceGetDynamicBoostLimit:\n");
        uint32_t buf[4] = {0};
        nvmlReturn_t ret = getBoost(device, buf);
        printf("    ret=%d, val=%u\n", ret, buf[0]);
    }

    /* Try GetNumGpuCores */
    typedef nvmlReturn_t (*fn_get_uint)(nvmlDevice_t, unsigned int *);
    fn_get_uint getCores = (fn_get_uint)dlsym(lib_handle, "nvmlDeviceGetNumGpuCores");
    if (getCores) {
        unsigned int cores = 0;
        nvmlReturn_t ret = getCores(device, &cores);
        printf("  NumGpuCores: %u (ret=%d)\n", cores, ret);
    }

    fn_get_uint getBusType = (fn_get_uint)dlsym(lib_handle, "nvmlDeviceGetBusType");
    if (getBusType) {
        unsigned int bt = 0;
        nvmlReturn_t ret = getBusType(device, &bt);
        printf("  BusType: %u (ret=%d)\n", bt, ret);
    }

    fn_get_uint getArch = (fn_get_uint)dlsym(lib_handle, "nvmlDeviceGetArchitecture");
    if (getArch) {
        unsigned int arch = 0;
        nvmlReturn_t ret = getArch(device, &arch);
        printf("  Architecture: %u (ret=%d)\n", arch, ret);
    }

    fn_get_uint getPowerSource = (fn_get_uint)dlsym(lib_handle, "nvmlDeviceGetPowerSource");
    if (getPowerSource) {
        unsigned int ps = 0;
        nvmlReturn_t ret = getPowerSource(device, &ps);
        printf("  PowerSource: %u (ret=%d)\n", ps, ret);
    }

    fn_get_uint getMemBusWidth = (fn_get_uint)dlsym(lib_handle, "nvmlDeviceGetMemoryBusWidth");
    if (getMemBusWidth) {
        unsigned int bw = 0;
        nvmlReturn_t ret = getMemBusWidth(device, &bw);
        printf("  MemoryBusWidth: %u bits (ret=%d)\n", bw, ret);
    }

    fn_get_uint getAdaptive = (fn_get_uint)dlsym(lib_handle, "nvmlDeviceGetAdaptiveClockInfoStatus");
    if (getAdaptive) {
        unsigned int ac = 0;
        nvmlReturn_t ret = getAdaptive(device, &ac);
        printf("  AdaptiveClockInfoStatus: %u (ret=%d)\n", ac, ret);
    }

    /* GSP firmware */
    typedef nvmlReturn_t (*fn_gsp_ver)(nvmlDevice_t, char *, unsigned int);
    fn_gsp_ver getGspVer = (fn_gsp_ver)dlsym(lib_handle, "nvmlDeviceGetGspFirmwareVersion");
    if (getGspVer) {
        char gsp_buf[256] = {0};
        nvmlReturn_t ret = getGspVer(device, gsp_buf, sizeof(gsp_buf));
        printf("  GspFirmwareVersion: '%s' (ret=%d)\n", gsp_buf, ret);
    }

    typedef nvmlReturn_t (*fn_gsp_mode)(nvmlDevice_t, unsigned int *, unsigned int *);
    fn_gsp_mode getGspMode = (fn_gsp_mode)dlsym(lib_handle, "nvmlDeviceGetGspFirmwareMode");
    if (getGspMode) {
        unsigned int enabled = 0, default_mode = 0;
        nvmlReturn_t ret = getGspMode(device, &enabled, &default_mode);
        printf("  GspFirmwareMode: enabled=%u, default=%u (ret=%d)\n", enabled, default_mode, ret);
    }

    /* PCIe info */
    typedef nvmlReturn_t (*fn_pcie)(nvmlDevice_t, unsigned int, unsigned int *);
    fn_pcie getPcieThroughput = (fn_pcie)dlsym(lib_handle, "nvmlDeviceGetPcieThroughput");
    if (getPcieThroughput) {
        for (unsigned int counter = 0; counter < 3; counter++) {
            unsigned int val = 0;
            nvmlReturn_t ret = getPcieThroughput(device, counter, &val);
            const char *names[] = {"TX_BYTES", "RX_BYTES", "PACKET_THROUGHPUT"};
            printf("  PcieThroughput[%s]: %u (ret=%d)\n",
                   counter < 3 ? names[counter] : "?", val, ret);
        }
    }

    fn_get_uint getMaxPcieGen = (fn_get_uint)dlsym(lib_handle, "nvmlDeviceGetMaxPcieLinkGeneration");
    fn_get_uint getMaxPcieWidth = (fn_get_uint)dlsym(lib_handle, "nvmlDeviceGetMaxPcieLinkWidth");
    fn_get_uint getCurPcieGen = (fn_get_uint)dlsym(lib_handle, "nvmlDeviceGetCurrPcieLinkGeneration");
    fn_get_uint getCurPcieWidth = (fn_get_uint)dlsym(lib_handle, "nvmlDeviceGetCurrPcieLinkWidth");

    unsigned int val = 0;
    if (getMaxPcieGen) { getMaxPcieGen(device, &val); printf("  MaxPCIeGen: %u\n", val); }
    if (getMaxPcieWidth) { getMaxPcieWidth(device, &val); printf("  MaxPCIeWidth: x%u\n", val); }
    if (getCurPcieGen) { getCurPcieGen(device, &val); printf("  CurPCIeGen: %u\n", val); }
    if (getCurPcieWidth) { getCurPcieWidth(device, &val); printf("  CurPCIeWidth: x%u\n", val); }
}

/* ─── Discover ALL exported NVML symbols we haven't tried ─── */
static void discover_remaining_exports(void *lib_handle) {
    printf("\n══════════════════════════════════════════════\n");
    printf("  Scanning for additional undocumented functions\n");
    printf("══════════════════════════════════════════════\n");

    /* We'll read the nm output from the shared library */
    FILE *fp = popen("nm -D /usr/lib64/libnvidia-ml.so.1 2>/dev/null | grep ' T ' | awk '{print $3}' | sort", "r");
    if (!fp) return;

    char line[512];
    int count = 0;
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\n")] = 0;
        /* Skip well-known documented functions */
        if (strstr(line, "@@")) {
            char *at = strstr(line, "@@");
            *at = 0;
        }
        void *fn = dlsym(lib_handle, line);
        if (fn) count++;
    }
    pclose(fp);
    printf("  Total exported T symbols with dlsym resolution: %d\n", count);
}

int main(void) {
    printf("╔══════════════════════════════════════════════╗\n");
    printf("║  Deep NVML Undocumented API Probe v2         ║\n");
    printf("╠══════════════════════════════════════════════╣\n");

    lib = dlopen("libnvidia-ml.so.1", RTLD_NOW);
    if (!lib) { fprintf(stderr, "dlopen failed: %s\n", dlerror()); return 1; }

    fn_nvmlInit_v2 init = (fn_nvmlInit_v2)dlsym(lib, "nvmlInit_v2");
    fn_nvmlShutdown shutdown = (fn_nvmlShutdown)dlsym(lib, "nvmlShutdown");
    fn_nvmlDeviceGetHandleByIndex_v2 getHandle =
        (fn_nvmlDeviceGetHandleByIndex_v2)dlsym(lib, "nvmlDeviceGetHandleByIndex_v2");
    fn_nvmlDeviceGetName getName = (fn_nvmlDeviceGetName)dlsym(lib, "nvmlDeviceGetName");
    errStr = (fn_nvmlErrorString)dlsym(lib, "nvmlErrorString");

    if (!init || !getHandle) { fprintf(stderr, "Missing critical symbols\n"); return 1; }

    nvmlReturn_t ret = init();
    if (ret != 0) { fprintf(stderr, "nvmlInit failed: %d\n", ret); return 1; }

    ret = getHandle(0, &device);
    if (ret != 0) { fprintf(stderr, "getHandle failed: %d\n", ret); return 1; }

    char name[256];
    getName(device, name, sizeof(name));
    printf("║  GPU: %-40s  ║\n", name);
    printf("╚══════════════════════════════════════════════╝\n");

    /* 1. Deep probe already-working functions */
    deep_probe_dynamic_pstates(
        (fn_dev_struct)dlsym(lib, "nvmlDeviceGetDynamicPstatesInfo"));

    deep_probe_gpc_clk_vf_offset(lib);
    deep_probe_power_mizer(lib);
    deep_probe_fan_apis(lib);

    /* 2. Brute-force version-mismatched functions */
    fn_dev_struct fn;

    fn = (fn_dev_struct)dlsym(lib, "nvmlDeviceGetCoolerInfo");
    if (fn) probe_cooler_info(fn);

    fn = (fn_dev_struct)dlsym(lib, "nvmlDeviceGetPerformanceModes");
    if (fn) probe_performance_modes(fn);

    fn = (fn_dev_struct)dlsym(lib, "nvmlDeviceGetMarginTemperature");
    if (fn) probe_margin_temperature(fn);

    fn = (fn_dev_struct)dlsym(lib, "nvmlDeviceWorkloadPowerProfileGetProfilesInfo");
    if (fn) probe_workload_power_profiles_info(fn);

    fn = (fn_dev_struct)dlsym(lib, "nvmlDeviceWorkloadPowerProfileGetCurrentProfiles");
    if (fn) probe_workload_power_current(fn);

    /* 3. Power smoothing */
    deep_probe_power_smoothing(lib);

    /* 4. WorkloadPowerProfile (all variants) */
    deep_probe_workload_power(lib);

    /* 5. Additional undocumented functions */
    probe_all_undocumented_nvml(lib);

    shutdown();
    dlclose(lib);

    printf("\n\n════ PROBE COMPLETE ════\n");
    return 0;
}
