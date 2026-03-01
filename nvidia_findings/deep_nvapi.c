/*
 * Deep NvAPI investigation probe
 * Disassemble-guided struct decoding for key NvAPI undocumented functions.
 *
 * Build: gcc -O0 -g -o /tmp/deep_nvapi /tmp/deep_nvapi.c -ldl
 * Run:   /tmp/deep_nvapi 2>&1
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <stdint.h>

typedef int NvStatus;
typedef void *NvPhysicalGpuHandle;
typedef void *(*NvAPI_QueryInterface_t)(uint32_t id);

static NvAPI_QueryInterface_t QueryInterface;
static NvPhysicalGpuHandle gpu = NULL;

/* NvAPI function IDs */
#define NVAPI_INITIALIZE             0x0150E828
#define NVAPI_UNLOAD                 0xD22BDD7E
#define NVAPI_ENUMPHYSICALGPUS       0xE5AC921F
#define NVAPI_GETFULLNAME            0xCEEE8E9F
#define NVAPI_ERRORMESSAGE           0x6C2D048C

/* Interesting undocumented function IDs */
#define NVAPI_SETPSTATES20           0x0F4DAE6B
#define NVAPI_GETPSTATES20           0x6FF81213  /* often paired with set */
#define NVAPI_ALLCLOCKFREQ           0xDCB616C3
#define NVAPI_VOLTAGE                0x465F9BCF
#define NVAPI_GETPERFCLOCKS          0x1EA54A3B  /* or 0x409C4128 for v2 */
#define NVAPI_GETPERFCLOCKS_V2       0x409C4128
#define NVAPI_THERMALSENSORS         0x65FE3AAD
#define NVAPI_FAN_COOLERS_GET        0x18A519AC
#define NVAPI_FAN_COOLERS_SET_CTRL   0x814B209F
#define NVAPI_FAN_COOLERS_GET_CTRL   0xC4F3E3B4
#define NVAPI_FAN_COOLERS_GET_STATUS 0xDA141340
#define NVAPI_POWER_TOPO             0xEDECEEAF
#define NVAPI_POWER_POLICIES_STATUS  0x70916171
#define NVAPI_POWER_POLICIES_INFO    0x34206D86
#define NVAPI_PERF_POLICIES_STATUS   0x3D358A0C
#define NVAPI_COOLER_SETTINGS        0xDA141340
#define NVAPI_GET_SHORTNAME          0xD988F0F3

typedef NvStatus (*fn_void)(void);
typedef NvStatus (*fn_enum_gpus)(NvPhysicalGpuHandle *, int *);
typedef NvStatus (*fn_get_string)(NvPhysicalGpuHandle, char *);
typedef NvStatus (*fn_err_msg)(NvStatus, char *);
typedef NvStatus (*fn_gpu_struct)(NvPhysicalGpuHandle, void *);

static fn_err_msg ErrMsg;

static void hexdump(const void *data, size_t bytes) {
    const uint8_t *p = data;
    for (size_t i = 0; i < bytes; i++) {
        if (i % 32 == 0) printf("    %04zx: ", i);
        printf("%02x", p[i]);
        if (i % 4 == 3) printf(" ");
        if (i % 32 == 31) printf("\n");
    }
    if (bytes % 32) printf("\n");
}

static void hexdump_nonzero(const void *data, size_t total) {
    const uint8_t *p = data;
    size_t last = 0;
    for (size_t i = 0; i < total; i++) {
        if (p[i]) last = i;
    }
    size_t show = ((last + 32) / 32) * 32;
    if (show < 64) show = 64;
    if (show > total) show = total;
    hexdump(data, show);
}

/* Make NvAPI struct version: size in lower 16 bits, version in upper 16 */
/* Actually NvAPI uses: version = (sizeof_struct) | (ver_number << 16) */
/* Some use: version = (sizeof_struct) | (ver_number << 24) like NVML */
/* This needs investigation - let's try both encodings */
static uint32_t nvapi_ver_16(uint32_t size, uint32_t ver) {
    return (size & 0xFFFF) | (ver << 16);
}
static uint32_t nvapi_ver_24(uint32_t size, uint32_t ver) {
    return (size & 0xFFFFFF) | (ver << 24);
}

/* ─── Brute force NvAPI function versions ─── */
static int brute_nvapi(const char *name, uint32_t func_id,
                       uint32_t min_sz, uint32_t max_sz, uint32_t step,
                       int max_ver) {
    fn_gpu_struct fn = (fn_gpu_struct)QueryInterface(func_id);
    if (!fn) {
        printf("  %s (0x%08x): NOT AVAILABLE\n", name, func_id);
        return 0;
    }
    printf("\n══ %s (0x%08x) ══\n", name, func_id);

    uint8_t buf[65536];
    int found = 0;

    /* Try ver<<16 encoding (NvAPI standard) */
    for (uint32_t sz = min_sz; sz <= max_sz; sz += step) {
        for (int v = 1; v <= max_ver; v++) {
            uint32_t ver16 = nvapi_ver_16(sz, v);
            memset(buf, 0, sizeof(buf));
            *(uint32_t *)buf = ver16;
            NvStatus ret = fn(gpu, buf);
            if (ret == 0) {
                printf("  ✓ v16: size=%u ver=%d (0x%08x)\n", sz, v, ver16);
                printf("    Data:\n");
                hexdump_nonzero(buf, sz);
                found = 1;
            } else if (ret != -9) {
                /* -9 = INCOMPATIBLE_STRUCT_VERSION */
                char msg[256] = {0};
                if (ErrMsg) ErrMsg(ret, msg);
                printf("  v16: sz=%u v=%d => %d (%s)\n", sz, v, ret, msg);
            }
        }
    }

    /* Try ver<<24 encoding (like NVML) */
    for (uint32_t sz = min_sz; sz <= max_sz; sz += step) {
        for (int v = 1; v <= max_ver; v++) {
            uint32_t ver24 = nvapi_ver_24(sz, v);
            memset(buf, 0, sizeof(buf));
            *(uint32_t *)buf = ver24;
            NvStatus ret = fn(gpu, buf);
            if (ret == 0) {
                printf("  ✓ v24: size=%u ver=%d (0x%08x)\n", sz, v, ver24);
                printf("    Data:\n");
                hexdump_nonzero(buf, sz);
                found = 1;
            }
            /* Skip logging for INCOMPATIBLE */
        }
    }

    if (!found) printf("  No working version found.\n");
    return found;
}

/* ─── Deep probe Voltage (already known: size=76, ver=1, 0x0001004c) ─── */
static void probe_voltage(void) {
    fn_gpu_struct fn = (fn_gpu_struct)QueryInterface(NVAPI_VOLTAGE);
    if (!fn) { printf("Voltage: NOT AVAILABLE\n"); return; }

    printf("\n══ Deep Voltage Probe (0x465F9BCF) ══\n");

    uint8_t buf[256];
    memset(buf, 0, sizeof(buf));
    *(uint32_t *)buf = nvapi_ver_16(76, 1); /* Known working */

    NvStatus ret = fn(gpu, buf);
    if (ret == 0) {
        printf("  SUCCESS! Full 76-byte struct:\n");
        hexdump(buf, 76);

        uint32_t *u32 = (uint32_t *)buf;
        printf("\n  Field analysis:\n");
        printf("    [0x00] version: 0x%08x\n", u32[0]);
        printf("    [0x04] field1:  0x%08x (%u)\n", u32[1], u32[1]);
        printf("    [0x08] field2:  0x%08x (%u)\n", u32[2], u32[2]);
        printf("    [0x0c] field3:  0x%08x (%u)\n", u32[3], u32[3]);
        printf("    [0x10] field4:  0x%08x (%u)\n", u32[4], u32[4]);
        printf("    [0x14] field5:  0x%08x (%u)\n", u32[5], u32[5]);
        printf("    [0x18] field6:  0x%08x (%u)\n", u32[6], u32[6]);
        printf("    [0x1c] field7:  0x%08x (%u)\n", u32[7], u32[7]);
        printf("    [0x20] field8:  0x%08x (%u)\n", u32[8], u32[8]);
        printf("    [0x24] field9:  0x%08x (%u)\n", u32[9], u32[9]);
        printf("    [0x28] voltage: 0x%08x (%u µV = %.3f V)\n",
               u32[10], u32[10], u32[10] / 1000000.0);
        for (int i = 11; i < 19; i++) {
            if (u32[i] != 0)
                printf("    [0x%02x] field%d: 0x%08x (%u)\n", i*4, i, u32[i], u32[i]);
        }
    }
}

/* ─── Deep probe AllClockFrequencies (known: size=264, ver=1) ─── */
static void probe_clocks(void) {
    fn_gpu_struct fn = (fn_gpu_struct)QueryInterface(NVAPI_ALLCLOCKFREQ);
    if (!fn) { printf("AllClockFreq: NOT AVAILABLE\n"); return; }

    printf("\n══ Deep AllClockFrequencies Probe (0xDCB616C3) ══\n");

    /* Try multiple versions to see which gives most data */
    for (int v = 1; v <= 3; v++) {
        for (int sz = 264; sz <= 600; sz += 8) {
            uint8_t buf[4096];
            memset(buf, 0, sizeof(buf));
            *(uint32_t *)buf = nvapi_ver_16(sz, v);

            NvStatus ret = fn(gpu, buf);
            if (ret == 0) {
                printf("\n  v%d sz=%d: SUCCESS\n", v, sz);

                /* Find last non-zero */
                size_t last = 0;
                for (size_t i = 4; i < (size_t)sz; i++) { if (buf[i]) last = i; }
                printf("    Last non-zero at offset %zu\n", last);

                if (sz == 264 && v == 1) {
                    hexdump(buf, 264);

                    uint32_t *u = (uint32_t *)buf;
                    printf("\n  Parsed AllClockFrequencies:\n");
                    printf("    version: 0x%08x\n", u[0]);

                    /* NV_GPU_CLOCK_FREQUENCIES structure:
                     * offset 4: ClockType (0=current, 1=base, 2=boost, 3=tdp)
                     * offset 8: reserved[8]
                     * offset 40+: domain entries, each 12 bytes:
                     *   uint32_t bIsPresent
                     *   uint32_t frequency_kHz
                     *   uint32_t reserved (or voltage?)
                     */
                    printf("    [0x04] clockType: %u\n", u[1]);
                    for (int i = 0; i < 4; i++) {
                        printf("    [0x%02x] reserved[%d]: 0x%08x\n", 8+(i*4), i, u[2+i]);
                    }

                    /* Scan for domain entries starting at various offsets */
                    printf("\n  Looking for clock domain entries:\n");
                    for (size_t off = 8; off < 260; off += 4) {
                        uint32_t val = *(uint32_t*)(buf + off);
                        if (val != 0) {
                            printf("    offset 0x%03zx: 0x%08x (%u", off, val, val);
                            if (val > 10000 && val < 10000000)
                                printf(", %.1f MHz", val / 1000.0);
                            printf(")\n");
                        }
                    }
                }
                break; /* Found working size for this version */
            }
        }
    }

    /* Try fetching different clock types: current(0), base(1), boost(2) */
    printf("\n  Fetching different clock types:\n");
    for (int ct = 0; ct <= 3; ct++) {
        uint8_t buf[4096];
        memset(buf, 0, sizeof(buf));
        *(uint32_t *)buf = nvapi_ver_16(264, 1);
        *(uint32_t *)(buf + 4) = ct; /* Set clockType */

        NvStatus ret = fn(gpu, buf);
        if (ret == 0) {
            const char *type_names[] = {"CURRENT", "BASE", "BOOST", "TDP"};
            printf("    ClockType %s:\n", ct < 4 ? type_names[ct] : "?");

            /* Print all non-zero fields */
            for (size_t off = 8; off < 264; off += 4) {
                uint32_t val = *(uint32_t*)(buf + off);
                if (val != 0) {
                    printf("      offset 0x%03zx: %u", off, val);
                    if (val > 10000 && val < 10000000)
                        printf(" (%.1f MHz)", val / 1000.0);
                    printf("\n");
                }
            }
        } else {
            char msg[256] = {0};
            if (ErrMsg) ErrMsg(ret, msg);
            printf("    ClockType %d: ret=%d (%s)\n", ct, ret, msg);
        }
    }
}

/* ─── Deep probe SetPstates20 ─── */
static void probe_pstates20(void) {
    fn_gpu_struct fn_get = (fn_gpu_struct)QueryInterface(NVAPI_GETPSTATES20);
    fn_gpu_struct fn_set = (fn_gpu_struct)QueryInterface(NVAPI_SETPSTATES20);

    printf("\n══ Pstates20 Investigation ══\n");
    printf("  GetPstates20 (0x6FF81213): %s\n", fn_get ? "AVAILABLE" : "NOT FOUND");
    printf("  SetPstates20 (0x0F4DAE6B): %s\n", fn_set ? "AVAILABLE" : "NOT FOUND");

    if (fn_get) {
        /* NV_GPU_PERF_PSTATES20_INFO structure from NvAPI SDK:
         * Version 1: 0x11c34 (72500 bytes!) or version 2
         * Let's try various sizes.
         * Typical: version field + flags + numPstates + numClocks + numBaseVoltages
         * + pstates[16] array where each pstate has clocks and voltages
         */
        printf("\n  Brute-forcing GetPstates20 versions:\n");

        /* Based on NvAPI headers, NV_GPU_PERF_PSTATES20_INFO size is:
         * v1: struct size varies by NV_MAX_GPU_PERF_PSTATES and other defines
         * Try sizes: 420, 500, 1000, 1500, 2000, etc */
        static const uint32_t try_sizes[] = {
            56, 64, 72, 80, 88, 96, 104, 112, 120, 128,
            160, 192, 224, 256, 288, 320,
            352, 384, 416, 448, 480, 512,
            544, 576, 608, 640, 672, 704, 736, 768,
            800, 896, 1024, 1152, 1280, 1408, 1536,
            1664, 1792, 1920, 2048, 2176, 2304, 2432, 2560,
            3072, 3584, 4096, 5120, 6144, 8192, 10240, 12288,
            16384, 0
        };

        for (int i = 0; try_sizes[i]; i++) {
            for (int v = 1; v <= 3; v++) {
                uint8_t buf[65536];
                uint32_t sz = try_sizes[i];
                memset(buf, 0, sizeof(buf));

                /* Try v<<16 encoding */
                *(uint32_t *)buf = nvapi_ver_16(sz, v);
                NvStatus ret = fn_get(gpu, buf);
                if (ret == 0) {
                    printf("  ✓ GetPstates20: v16 sz=%u v=%d (0x%08x)\n",
                           sz, v, *(uint32_t *)buf);
                    size_t last = 0;
                    for (size_t j = 4; j < sz; j++) { if (buf[j]) last = j; }
                    printf("    Last non-zero at %zu\n", last);
                    hexdump(buf, last + 32 < sz ? last + 32 : sz);
                } else if (ret != -9) {
                    char msg[256] = {0};
                    if (ErrMsg) ErrMsg(ret, msg);
                    printf("  v16 sz=%u v=%d => %d (%s)\n", sz, v, ret, msg);
                }

                /* Try v<<24 encoding */
                *(uint32_t *)buf = nvapi_ver_24(sz, v);
                ret = fn_get(gpu, buf);
                if (ret == 0) {
                    printf("  ✓ GetPstates20: v24 sz=%u v=%d (0x%08x)\n",
                           sz, v, *(uint32_t *)buf);
                    size_t last = 0;
                    for (size_t j = 4; j < sz; j++) { if (buf[j]) last = j; }
                    printf("    Last non-zero at %zu\n", last);
                    hexdump(buf, last + 32 < sz ? last + 32 : sz);
                }
            }
        }
    }

    /* Also scan NVAPI functions we haven't fully explored */
    printf("\n  Additional NvAPI ID scans:\n");

    /* Try known NvAPI IDs for performance/clock-related functions */
    static const struct { uint32_t id; const char *name; } extra_ids[] = {
        {0x6FF81213, "GetPstates20_v1"},
        {0x1EA54A3B, "GetPerfClocks_v1"},
        {0x409C4128, "GetPerfClocks_v2"},
        {0x34206D86, "PowerPoliciesGetInfo"},
        {0x70916171, "PowerPoliciesGetStatus"},
        {0xAD95F5ED, "PowerPoliciesSetStatus"},
        {0xEDECEEAF, "PowerTopoGetInfo"},
        {0x3D358A0C, "PerfPoliciesGetStatus"},
        {0xDA141340, "FanCoolerGetStatus"},
        {0x18A519AC, "FanCoolersGet"},
        {0x814B209F, "FanCoolersSetControl"},
        {0xC4F3E3B4, "FanCoolersGetControl"},
        {0x340AB3F2, "FanCoolersGetInfo"},
        {0xEFC3BF0E, "FanCoolersInfo?"},
        {0x60DED2ED, "ClientPowerTopologyGetInfo"},
        {0, NULL}
    };

    for (int i = 0; extra_ids[i].name; i++) {
        brute_nvapi(extra_ids[i].name, extra_ids[i].id,
                    32, 2048, 8, 5);
    }

    /* FanCooler functions with higher version/size range */
    printf("\n  Extended FanCooler scans:\n");
    brute_nvapi("FanCoolersGet_ext", NVAPI_FAN_COOLERS_GET, 32, 8192, 16, 5);
}

/* ─── Probe all NvAPI thermal/power functions ─── */
static void probe_thermal_power(void) {
    printf("\n══ NvAPI Thermal/Power Investigation ══\n");

    /* ThermalSensors with exact driver-reported versions from previous session:
     * v1=0x1003c(60B), v2=0x200a8(168B), v3=0x334c8(13512B)
     * All returned DATA_NOT_FOUND on Blackwell. Let's confirm and try more. */
    fn_gpu_struct fn_thermal = (fn_gpu_struct)QueryInterface(NVAPI_THERMALSENSORS);
    if (fn_thermal) {
        printf("\n  ThermalSensors (0x65FE3AAD):\n");
        static const uint32_t exact_versions[] = {
            0x0001003c, /* v1, 60 bytes - from driver stderr */
            0x000200a8, /* v2, 168 bytes */
            0x000334c8, /* v3, 13512 bytes */
            0
        };
        for (int i = 0; exact_versions[i]; i++) {
            uint8_t buf[16384];
            memset(buf, 0, sizeof(buf));
            *(uint32_t *)buf = exact_versions[i];
            uint32_t sz = exact_versions[i] & 0xFFFFFF;
            NvStatus ret = fn_thermal(gpu, buf);
            char msg[256] = {0};
            if (ErrMsg) ErrMsg(ret, msg);
            printf("    ver=0x%08x (size=%u): ret=%d (%s)\n",
                   exact_versions[i], sz, ret, msg);
            if (ret == 0) {
                hexdump_nonzero(buf, sz > 256 ? 256 : sz);
            }
        }
    }

    /* PowerTopology */
    brute_nvapi("PowerTopoGetInfo", NVAPI_POWER_TOPO, 32, 2048, 8, 5);
    brute_nvapi("PowerPoliciesGetInfo", NVAPI_POWER_POLICIES_INFO, 32, 2048, 8, 5);
    brute_nvapi("PowerPoliciesGetStatus", NVAPI_POWER_POLICIES_STATUS, 32, 2048, 8, 5);
    brute_nvapi("PerfPoliciesGetStatus", NVAPI_PERF_POLICIES_STATUS, 32, 2048, 8, 5);
}

int main(void) {
    printf("╔══════════════════════════════════════════════╗\n");
    printf("║  Deep NvAPI Investigation Probe              ║\n");
    printf("╠══════════════════════════════════════════════╣\n");

    void *api_lib = dlopen("libnvidia-api.so.1", RTLD_NOW);
    if (!api_lib) { fprintf(stderr, "dlopen libnvidia-api: %s\n", dlerror()); return 1; }

    QueryInterface = dlsym(api_lib, "nvapi_QueryInterface");
    if (!QueryInterface) { fprintf(stderr, "nvapi_QueryInterface not found\n"); return 1; }

    /* Initialize */
    fn_void init = (fn_void)QueryInterface(NVAPI_INITIALIZE);
    if (!init || init() != 0) { fprintf(stderr, "NvAPI Initialize failed\n"); return 1; }

    /* Get GPU handle */
    fn_enum_gpus enumGpus = (fn_enum_gpus)QueryInterface(NVAPI_ENUMPHYSICALGPUS);
    NvPhysicalGpuHandle gpus[64];
    int count = 0;
    if (!enumGpus || enumGpus(gpus, &count) != 0 || count == 0) {
        fprintf(stderr, "No GPUs found\n"); return 1;
    }
    gpu = gpus[0];

    fn_get_string getName = (fn_get_string)QueryInterface(NVAPI_GETFULLNAME);
    ErrMsg = (fn_err_msg)QueryInterface(NVAPI_ERRORMESSAGE);

    char name[256] = {0};
    if (getName) getName(gpu, name);
    printf("║  GPU: %-40s  ║\n", name);
    printf("╚══════════════════════════════════════════════╝\n");

    /* Run deep investigations */
    probe_voltage();
    probe_clocks();
    probe_pstates20();
    probe_thermal_power();

    /* Cleanup */
    fn_void unload = (fn_void)QueryInterface(NVAPI_UNLOAD);
    if (unload) unload();
    dlclose(api_lib);

    printf("\n\n════ NVAPI PROBE COMPLETE ════\n");
    return 0;
}
