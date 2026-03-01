/*
 * Targeted deep field mapping of all working NvAPI/NVML undocumented functions.
 * This program dumps every byte of every working call for struct analysis.
 *
 * Build: gcc -O0 -g -o /tmp/deep_fields /tmp/deep_fields.c -ldl
 * Run:   /tmp/deep_fields 2>&1
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <stdint.h>

typedef void *nvmlDevice_t;
typedef int nvmlReturn_t;
typedef int NvStatus;
typedef void *NvPhysicalGpuHandle;
typedef void *(*NvAPI_QueryInterface_t)(uint32_t id);

static void hexdump(const void *data, size_t len) {
    const uint8_t *p = data;
    for (size_t i = 0; i < len; i += 16) {
        printf("    %04zx: ", i);
        for (size_t j = 0; j < 16 && i + j < len; j++)
            printf("%02x ", p[i + j]);
        for (size_t j = (len - i) < 16 ? (len - i) : 16; j < 16; j++)
            printf("   ");
        printf(" |");
        for (size_t j = 0; j < 16 && i + j < len; j++) {
            uint8_t c = p[i + j];
            printf("%c", (c >= 32 && c < 127) ? c : '.');
        }
        printf("|\n");
    }
}

static void print_u32_fields(const void *data, size_t len) {
    const uint32_t *p = data;
    for (size_t i = 0; i < len / 4; i++) {
        if (p[i] != 0)
            printf("    [0x%03zx] = 0x%08x (%u / %d)\n",
                   i * 4, p[i], p[i], (int32_t)p[i]);
    }
}

int main(void) {
    /* ——— NVML ——— */
    void *nvml = dlopen("libnvidia-ml.so.1", RTLD_NOW);
    nvmlDevice_t nvml_dev = NULL;
    if (nvml) {
        typedef nvmlReturn_t (*fn_init)(void);
        typedef nvmlReturn_t (*fn_getHandle)(unsigned int, nvmlDevice_t *);
        fn_init init = dlsym(nvml, "nvmlInit_v2");
        fn_getHandle getH = dlsym(nvml, "nvmlDeviceGetHandleByIndex_v2");
        if (init && init() == 0 && getH && getH(0, &nvml_dev) == 0) {
            printf("NVML initialized OK\n\n");
        }
    }

    /* ——— NvAPI ——— */
    void *nvapi_lib = dlopen("libnvidia-api.so.1", RTLD_NOW);
    NvAPI_QueryInterface_t QI = NULL;
    NvPhysicalGpuHandle gpu = NULL;
    if (nvapi_lib) {
        QI = dlsym(nvapi_lib, "nvapi_QueryInterface");
        if (QI) {
            NvStatus (*NvAPI_Initialize)(void) = QI(0x0150E828);
            NvStatus (*NvAPI_EnumPhysicalGPUs)(NvPhysicalGpuHandle[64], int *) = QI(0xE5AC921F);
            if (NvAPI_Initialize && NvAPI_Initialize() == 0) {
                NvPhysicalGpuHandle gpus[64];
                int count = 0;
                if (NvAPI_EnumPhysicalGPUs && NvAPI_EnumPhysicalGPUs(gpus, &count) == 0 && count > 0) {
                    gpu = gpus[0];
                    printf("NvAPI initialized OK, %d GPU(s)\n\n", count);
                }
            }
        }
    }

    uint8_t buf[65536] __attribute__((aligned(4096)));

    /* ═══ NVML: PerformanceModes (text-based, parsed) ═══ */
    if (nvml_dev) {
        printf("╔═══════════════════════════════════════════════╗\n");
        printf("║  NVML: PerformanceModes                       ║\n");
        printf("╚═══════════════════════════════════════════════╝\n");
        typedef nvmlReturn_t (*fn_dev_struct)(nvmlDevice_t, void *);
        fn_dev_struct fn = dlsym(nvml, "nvmlDeviceGetPerformanceModes");
        if (fn) {
            memset(buf, 0, sizeof(buf));
            *(uint32_t *)buf = (2052 & 0x00FFFFFF) | (1 << 24);
            nvmlReturn_t ret = fn(nvml_dev, buf);
            printf("  ret=%d\n", ret);
            if (ret == 0) {
                printf("\n  Parsed P-States:\n");
                /* Parse the text format */
                char *text = (char *)(buf + 4);
                char *saveptr, *token;
                char *copy = strdup(text);
                token = strtok_r(copy, ";", &saveptr);
                while (token) {
                    /* Trim leading whitespace */
                    while (*token == ' ') token++;
                    printf("    %s\n", token);
                    token = strtok_r(NULL, ";", &saveptr);
                }
                free(copy);
            }
        }
    }

    /* ═══ NVML: MarginTemperature ═══ */
    if (nvml_dev) {
        printf("\n╔═══════════════════════════════════════════════╗\n");
        printf("║  NVML: MarginTemperature                      ║\n");
        printf("╚═══════════════════════════════════════════════╝\n");
        typedef nvmlReturn_t (*fn_dev_struct)(nvmlDevice_t, void *);
        fn_dev_struct fn = dlsym(nvml, "nvmlDeviceGetMarginTemperature");
        if (fn) {
            memset(buf, 0, sizeof(buf));
            *(uint32_t *)buf = (8 & 0x00FFFFFF) | (1 << 24);
            nvmlReturn_t ret = fn(nvml_dev, buf);
            printf("  ret=%d, value=%u °C\n", ret, *(uint32_t *)(buf + 4));
            printf("  Struct layout: { uint32 version; uint32 margin_temp_C; }\n");
        }
    }

    /* ═══ NVML: DynamicPstatesInfo (no version) ═══ */
    if (nvml_dev) {
        printf("\n╔═══════════════════════════════════════════════╗\n");
        printf("║  NVML: DynamicPstatesInfo                     ║\n");
        printf("╚═══════════════════════════════════════════════╝\n");
        typedef nvmlReturn_t (*fn_dev_struct)(nvmlDevice_t, void *);
        fn_dev_struct fn = dlsym(nvml, "nvmlDeviceGetDynamicPstatesInfo");
        if (fn) {
            memset(buf, 0xCC, 256);
            nvmlReturn_t ret = fn(nvml_dev, buf);
            printf("  ret=%d\n  Non-zero uint32 fields:\n", ret);
            print_u32_fields(buf, 68);
            printf("  Struct: { uint32 flags; struct { uint32 bPresent; uint32 percentage; } slots[8]; }\n");
            printf("  GPU util: %u%%\n", *(uint32_t *)(buf + 8));
            printf("  FB util:  %u%%\n", *(uint32_t *)(buf + 16));
            printf("  VID util: %u%%\n", *(uint32_t *)(buf + 24));
        }
    }

    /* ═══ NVML: PowerMizerMode_v1 ═══ */
    if (nvml_dev) {
        printf("\n╔═══════════════════════════════════════════════╗\n");
        printf("║  NVML: PowerMizerMode_v1                      ║\n");
        printf("╚═══════════════════════════════════════════════╝\n");
        typedef nvmlReturn_t (*fn_dev_struct)(nvmlDevice_t, void *);
        fn_dev_struct fn = dlsym(nvml, "nvmlDeviceGetPowerMizerMode_v1");
        if (fn) {
            memset(buf, 0xCC, 64);
            nvmlReturn_t ret = fn(nvml_dev, buf);
            printf("  ret=%d\n", ret);
            printf("  All 12 bytes:\n");
            hexdump(buf, 12);
            printf("  field[0]: %u\n", *(uint32_t *)buf);
            printf("  field[1]: %u\n", *(uint32_t *)(buf + 4));
            printf("  field[2] (mode): %u\n", *(uint32_t *)(buf + 8));
            /* Try Set */
            typedef nvmlReturn_t (*fn_set)(nvmlDevice_t, void *);
            fn_set setFn = dlsym(nvml, "nvmlDeviceSetPowerMizerMode_v1");
            if (setFn) {
                printf("  SetPowerMizerMode_v1: available\n");
            }
        }
    }

    /* ═══ NVML: VfOffset ranges ═══ */
    if (nvml_dev) {
        printf("\n╔═══════════════════════════════════════════════╗\n");
        printf("║  NVML: VfOffset APIs (Get/Set)                ║\n");
        printf("╚═══════════════════════════════════════════════╝\n");
        typedef nvmlReturn_t (*fn_get)(nvmlDevice_t, int *);
        fn_get getGpc = dlsym(nvml, "nvmlDeviceGetGpcClkVfOffset");
        fn_get getMem = dlsym(nvml, "nvmlDeviceGetMemClkVfOffset");
        typedef nvmlReturn_t (*fn_getRange)(nvmlDevice_t, int *, int *);
        fn_getRange getGpcR = dlsym(nvml, "nvmlDeviceGetGpcClkMinMaxVfOffset");
        fn_getRange getMemR = dlsym(nvml, "nvmlDeviceGetMemClkMinMaxVfOffset");
        int val, lo, hi;
        if (getGpc) { getGpc(nvml_dev, &val); printf("  GpcClkVfOffset:       %d MHz\n", val); }
        if (getGpcR) { getGpcR(nvml_dev, &lo, &hi); printf("  GpcClk range:         [%d, %d] MHz\n", lo, hi); }
        if (getMem) { getMem(nvml_dev, &val); printf("  MemClkVfOffset:       %d MHz\n", val); }
        if (getMemR) { getMemR(nvml_dev, &lo, &hi); printf("  MemClk range:         [%d, %d] MHz\n", lo, hi); }
    }

    /* ═══ NVML: ThermalSettings (no real version check) ═══ */
    if (nvml_dev) {
        printf("\n╔═══════════════════════════════════════════════╗\n");
        printf("║  NVML: ThermalSettings                        ║\n");
        printf("╚═══════════════════════════════════════════════╝\n");
        typedef nvmlReturn_t (*fn_dev_struct)(nvmlDevice_t, void *);
        fn_dev_struct fn = dlsym(nvml, "nvmlDeviceGetThermalSettings");
        if (fn) {
            memset(buf, 0, 1024);
            *(uint32_t *)buf = (256 & 0x00FFFFFF) | (1 << 24);
            nvmlReturn_t ret = fn(nvml_dev, buf);
            printf("  ret=%d\n  Non-zero u32 fields:\n", ret);
            print_u32_fields(buf, 256);
        }
    }

    /* ═══ NvAPI: AllClockFrequencies v1/v2/v3 comparison ═══ */
    if (gpu && QI) {
        printf("\n╔═══════════════════════════════════════════════╗\n");
        printf("║  NvAPI: AllClockFrequencies (0xDCB616C3)       ║\n");
        printf("╚═══════════════════════════════════════════════╝\n");
        typedef NvStatus (*fn_gpu_struct)(NvPhysicalGpuHandle, void *);
        fn_gpu_struct fn = QI(0xDCB616C3);
        if (fn) {
            for (int v = 1; v <= 3; v++) {
                memset(buf, 0, 512);
                *(uint32_t *)buf = (264 & 0xFFFF) | (v << 16);
                NvStatus ret = fn(gpu, buf);
                printf("  v%d: ret=%d\n", v, ret);
                if (ret == 0) {
                    printf("  Non-zero u32 fields:\n");
                    print_u32_fields(buf, 264);
                    printf("  Clock domains:\n");
                    /* Each domain is 32 bytes starting at offset 8: present(4) + freq_kHz(4) + pad(24) */
                    for (int d = 0; d < 8; d++) {
                        uint32_t *dom = (uint32_t *)(buf + 8 + d * 32);
                        if (dom[0])
                            printf("    domain[%d]: freq=%u kHz (%.1f MHz)\n",
                                   d, dom[1], dom[1] / 1000.0);
                    }
                }
            }

            /* Try clock types in v3 */
            printf("\n  Clock types in v3:\n");
            const char *ct_names[] = {"CURRENT", "BASE", "BOOST", "TDP"};
            for (int ct = 0; ct < 4; ct++) {
                memset(buf, 0, 512);
                *(uint32_t *)buf = (264 & 0xFFFF) | (3 << 16);
                *(uint32_t *)(buf + 4) = ct;
                NvStatus ret = fn(gpu, buf);
                if (ret == 0) {
                    printf("    %s:\n", ct_names[ct]);
                    for (int d = 0; d < 8; d++) {
                        uint32_t *dom = (uint32_t *)(buf + 8 + d * 32);
                        if (dom[0])
                            printf("      domain[%d]: freq=%u kHz (%.1f MHz)\n",
                                   d, dom[1], dom[1] / 1000.0);
                    }
                }
            }
        }
    }

    /* ═══ NvAPI: Voltage deep ═══ */
    if (gpu && QI) {
        printf("\n╔═══════════════════════════════════════════════╗\n");
        printf("║  NvAPI: Voltage (0x465F9BCF)                   ║\n");
        printf("╚═══════════════════════════════════════════════╝\n");
        typedef NvStatus (*fn_gpu_struct)(NvPhysicalGpuHandle, void *);
        fn_gpu_struct fn = QI(0x465F9BCF);
        if (fn) {
            memset(buf, 0, 256);
            *(uint32_t *)buf = (76 & 0xFFFF) | (1 << 16);
            NvStatus ret = fn(gpu, buf);
            printf("  ret=%d\n", ret);
            if (ret == 0) {
                printf("  Non-zero u32 fields:\n");
                print_u32_fields(buf, 76);
                printf("  Voltage: %.3f V\n", *(uint32_t *)(buf + 0x28) / 1000000.0);
            }
        }
    }

    /* ═══ NvAPI: PerfPoliciesGetStatus ═══ */
    if (gpu && QI) {
        printf("\n╔═══════════════════════════════════════════════╗\n");
        printf("║  NvAPI: PerfPoliciesGetStatus (0x3D358A0C)     ║\n");
        printf("╚═══════════════════════════════════════════════╝\n");
        typedef NvStatus (*fn_gpu_struct)(NvPhysicalGpuHandle, void *);
        fn_gpu_struct fn = QI(0x3D358A0C);
        if (fn) {
            memset(buf, 0, 2048);
            *(uint32_t *)buf = (1360 & 0xFFFF) | (1 << 16);
            NvStatus ret = fn(gpu, buf);
            printf("  ret=%d\n", ret);
            if (ret == 0) {
                printf("  Non-zero u32 fields (first 256 bytes):\n");
                print_u32_fields(buf, 256);
                printf("  Full hex (first 128 bytes):\n");
                hexdump(buf, 128);
            }
        }
    }

    /* ═══ NvAPI: ClientPowerTopologyGetInfo ═══ */
    if (gpu && QI) {
        printf("\n╔═══════════════════════════════════════════════╗\n");
        printf("║  NvAPI: ClientPowerTopologyGetInfo (0x60DED2ED)║\n");
        printf("╚═══════════════════════════════════════════════╝\n");
        typedef NvStatus (*fn_gpu_struct)(NvPhysicalGpuHandle, void *);
        fn_gpu_struct fn = QI(0x60DED2ED);
        if (fn) {
            memset(buf, 0, 256);
            *(uint32_t *)buf = (72 & 0xFFFF) | (1 << 16);
            NvStatus ret = fn(gpu, buf);
            printf("  ret=%d\n", ret);
            if (ret == 0) {
                printf("  All non-zero u32 fields:\n");
                print_u32_fields(buf, 72);
                printf("  Full hex:\n");
                hexdump(buf, 72);
            }
        }
    }

    /* ═══ NvAPI: PowerPoliciesGetInfo ═══ */
    if (gpu && QI) {
        printf("\n╔═══════════════════════════════════════════════╗\n");
        printf("║  NvAPI: PowerPoliciesGetInfo (0x34206D86)      ║\n");
        printf("╚═══════════════════════════════════════════════╝\n");
        typedef NvStatus (*fn_gpu_struct)(NvPhysicalGpuHandle, void *);
        fn_gpu_struct fn = QI(0x34206D86);
        if (fn) {
            memset(buf, 0, 512);
            *(uint32_t *)buf = (184 & 0xFFFF) | (1 << 16);
            NvStatus ret = fn(gpu, buf);
            printf("  ret=%d\n", ret);
            if (ret == 0) {
                printf("  All non-zero u32 fields:\n");
                print_u32_fields(buf, 184);
                printf("  Full hex:\n");
                hexdump(buf, 184);
            }
        }
    }

    /* ═══ NvAPI: PowerPoliciesGetStatus (v1 + v2) ═══ */
    if (gpu && QI) {
        printf("\n╔═══════════════════════════════════════════════╗\n");
        printf("║  NvAPI: PowerPoliciesGetStatus (0x70916171)    ║\n");
        printf("╚═══════════════════════════════════════════════╝\n");
        typedef NvStatus (*fn_gpu_struct)(NvPhysicalGpuHandle, void *);
        fn_gpu_struct fn = QI(0x70916171);
        if (fn) {
            for (int v = 1; v <= 2; v++) {
                uint32_t sz = v == 1 ? 72 : 1368;
                memset(buf, 0, 2048);
                *(uint32_t *)buf = (sz & 0xFFFF) | (v << 16);
                NvStatus ret = fn(gpu, buf);
                printf("  v%d (sz=%u): ret=%d\n", v, sz, ret);
                if (ret == 0) {
                    printf("    Non-zero u32 fields:\n");
                    print_u32_fields(buf, sz);
                }
            }
        }
    }

    /* ═══ NvAPI: Additional clock IDs ═══ */
    if (gpu && QI) {
        printf("\n╔═══════════════════════════════════════════════╗\n");
        printf("║  NvAPI: Misc GPU Clock/Perf queries            ║\n");
        printf("╚═══════════════════════════════════════════════╝\n");

        /* GetClockBoostLock */
        typedef NvStatus (*fn_gpu_struct)(NvPhysicalGpuHandle, void *);
        uint32_t misc_ids[] = {
            0xE440B867, /* GetClockBoostLock */
            0x507B4B59, /* GetClockBoostTable */
            0x7F5F90A7, /* GetVFPCurve */
            0x47AAD0D7, /* GetClockBoostRanges maybe */
            0x0D258BB5, /* GetPerfClocks_v3? */
            0x2DDFB66E, /* GpuGetPerfClocks */
        };
        const char *misc_names[] = {
            "GetClockBoostLock",
            "GetClockBoostTable",
            "GetVFPCurve",
            "unknown_47AAD0D7",
            "unknown_0D258BB5",
            "GpuGetPerfClocks_2DDFB66E",
        };
        for (int i = 0; i < 6; i++) {
            fn_gpu_struct fn = QI(misc_ids[i]);
            if (fn) {
                printf("  %s (0x%08X): AVAILABLE\n", misc_names[i], misc_ids[i]);
                /* Try small sizes */
                for (int sz = 8; sz <= 512; sz += 8) {
                    for (int v = 1; v <= 3; v++) {
                        memset(buf, 0, 1024);
                        *(uint32_t *)buf = (sz & 0xFFFF) | (v << 16);
                        NvStatus ret = fn(gpu, buf);
                        if (ret == 0) {
                            printf("    sz=%d v=%d: OK!\n", sz, v);
                            print_u32_fields(buf, sz < 128 ? sz : 128);
                            goto next_misc;
                        }
                    }
                }
                printf("    No working version found\n");
            } else {
                printf("  %s (0x%08X): not available\n", misc_names[i], misc_ids[i]);
            }
            next_misc:;
        }
    }

    printf("\n════ COMPLETE ════\n");
    return 0;
}
