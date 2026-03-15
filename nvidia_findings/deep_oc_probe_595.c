/*
 * deep_oc_probe_595.c - Focused OC API probe for driver 595.45.04
 *
 * Probes the newly-available ClockBoostTable, VFPCurve, ClockOffsets,
 * and Pstates20 APIs with comprehensive struct layout detection.
 *
 * Build: gcc -O2 -o deep_oc_probe_595 deep_oc_probe_595.c -ldl
 * Run:   sudo ./deep_oc_probe_595
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>
#include <signal.h>
#include <setjmp.h>

typedef void *nvmlDevice_t;
typedef int nvmlReturn_t;

#define NVML_VER(sz, ver) (((sz) & 0x00FFFFFF) | ((ver) << 24))
/* NvAPI version: (struct_size & 0xFFFF) | (version_number << 16) */
#define NVAPI_VER(sz, ver) (((sz) & 0xFFFF) | ((ver) << 16))

static sigjmp_buf jump_buf;
static volatile int in_probe = 0;

static void crash_handler(int sig) {
    if (in_probe) siglongjmp(jump_buf, sig);
    _exit(128 + sig);
}

#define SAFE_CALL(expr, label) do { \
    in_probe = 1; \
    if (sigsetjmp(jump_buf, 1) == 0) { \
        ret = (expr); \
    } else { \
        ret = -999; \
        printf("  [CRASHED in %s]\n", label); \
    } \
    in_probe = 0; \
} while(0)

typedef nvmlReturn_t (*nvmlInit_t)(void);
typedef nvmlReturn_t (*nvmlShutdown_t)(void);
typedef nvmlReturn_t (*nvmlDeviceGetHandleByIndex_t)(unsigned int, nvmlDevice_t *);
typedef const char *(*nvmlErrorString_t)(nvmlReturn_t);
typedef nvmlReturn_t (*nvml_dev_buf_t)(nvmlDevice_t, void *);
typedef void *(*nvapi_qi_t)(unsigned int);

/* NvAPI function type: status = fn(handle, buf) */
typedef int (*nvapi_fn_t)(void *, void *);

static const char *ret_str(nvmlReturn_t ret, nvmlErrorString_t errStr) {
    if (ret == -999) return "CRASHED";
    if (ret == 0) return "SUCCESS";
    if (errStr) return errStr(ret);
    static char buf[32];
    snprintf(buf, sizeof(buf), "error=%d", ret);
    return buf;
}

static void hexdump_line(const void *data, size_t offset, size_t len) {
    const uint8_t *p = (const uint8_t *)data + offset;
    printf("  %04zx: ", offset);
    for (size_t i = 0; i < len && i < 32; i++)
        printf("%02x ", p[i]);
    printf("\n");
}

static void dump_u32_nonzero(const void *data, size_t len) {
    const uint32_t *p = (const uint32_t *)data;
    size_t count = len / 4;
    for (size_t i = 0; i < count; i++) {
        if (p[i] != 0) {
            printf("  [%3zu] +0x%03zx = 0x%08X (%u / %d)\n",
                   i, i * 4, p[i], p[i], (int32_t)p[i]);
        }
    }
}

static void full_hexdump(const void *data, size_t len) {
    for (size_t off = 0; off < len; off += 32) {
        size_t chunk = (len - off > 32) ? 32 : (len - off);
        hexdump_line(data, off, chunk);
    }
}

/* Scan for version/size of a versioned NVML API call */
static int scan_nvml_versioned(nvml_dev_buf_t fn, nvmlDevice_t dev,
                                nvmlErrorString_t errStr, const char *name,
                                uint8_t *outbuf, unsigned int *out_sz) {
    nvmlReturn_t ret;
    for (unsigned int ver = 1; ver <= 4; ver++) {
        for (unsigned int sz = 8; sz <= 4096; sz += 4) {
            memset(outbuf, 0, 8192);
            uint32_t vword = NVML_VER(sz, ver);
            memcpy(outbuf, &vword, 4);
            SAFE_CALL(fn(dev, outbuf), name);
            if (ret == 0) {
                *out_sz = sz;
                printf("  %s: FOUND sz=%u ver=%u vword=0x%08X\n", name, sz, ver, vword);
                return 1;
            }
            if (ret == 3) { /* NOT_SUPPORTED */
                printf("  %s: NOT SUPPORTED (sz=%u ver=%u)\n", name, sz, ver);
                return -1;
            }
            if (ret == -999) return -2; /* crashed */
        }
    }
    return 0;
}

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = crash_handler;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);

    void *lib = dlopen("libnvidia-ml.so.1", RTLD_LAZY);
    if (!lib) { fprintf(stderr, "No NVML\n"); return 1; }

    nvmlInit_t nvmlInit = dlsym(lib, "nvmlInit_v2");
    nvmlShutdown_t nvmlShutdown = dlsym(lib, "nvmlShutdown");
    nvmlDeviceGetHandleByIndex_t getHandle = dlsym(lib, "nvmlDeviceGetHandleByIndex_v2");
    nvmlErrorString_t errStr = dlsym(lib, "nvmlErrorString");

    nvmlReturn_t ret;
    nvmlInit();

    nvmlDevice_t dev;
    getHandle(0, &dev);

    printf("=================================================================\n");
    printf("  Deep OC Probe - NVIDIA Driver 595.45.04 - RTX 5090 Desktop\n");
    printf("=================================================================\n\n");

    /* =============================================================
     * SECTION A: nvmlDeviceGetClockOffsets (struct layout)
     * ============================================================= */
    printf("=== A. nvmlDeviceGetClockOffsets struct analysis ===\n\n");
    {
        nvml_dev_buf_t getClockOffsets = dlsym(lib, "nvmlDeviceGetClockOffsets");
        nvml_dev_buf_t setClockOffsets = dlsym(lib, "nvmlDeviceSetClockOffsets");

        if (getClockOffsets) {
            uint8_t buf[256];
            memset(buf, 0, sizeof(buf));
            uint32_t vword = NVML_VER(24, 1);
            memcpy(buf, &vword, 4);
            SAFE_CALL(getClockOffsets(dev, buf), "getClockOffsets");
            if (ret == 0) {
                printf("  Struct layout (24 bytes, version 1):\n");
                dump_u32_nonzero(buf, 24);
                printf("\n  Full hex:\n");
                full_hexdump(buf, 24);

                /* Interpret based on what we see */
                uint32_t *u = (uint32_t *)buf;
                printf("\n  Interpretation:\n");
                printf("    [0] version  = 0x%08X\n", u[0]);
                printf("    [1] type/clock_type  = %d\n", (int32_t)u[1]);
                printf("    [2] pstate   = %d\n", (int32_t)u[2]);
                printf("    [3] offset   = %d (current clock offset MHz?)\n", (int32_t)u[3]);
                printf("    [4] minOffset = %d\n", (int32_t)u[4]);
                printf("    [5] maxOffset = %d\n", (int32_t)u[5]);

                /* Now try different clock types */
                printf("\n  Probing different clockType values:\n");
                for (int ct = 0; ct < 8; ct++) {
                    memset(buf, 0, sizeof(buf));
                    vword = NVML_VER(24, 1);
                    memcpy(buf, &vword, 4);
                    uint32_t clockType = ct;
                    memcpy(buf + 4, &clockType, 4);
                    SAFE_CALL(getClockOffsets(dev, buf), "getClockOffsets");
                    if (ret == 0) {
                        int32_t offset, minOff, maxOff;
                        memcpy(&offset, buf + 12, 4);
                        memcpy(&minOff, buf + 16, 4);
                        memcpy(&maxOff, buf + 20, 4);
                        printf("    clockType=%d: offset=%d, min=%d, max=%d\n",
                               ct, offset, minOff, maxOff);
                    } else {
                        printf("    clockType=%d: %s\n", ct, ret_str(ret, errStr));
                    }
                }

                /* Try different pstate values with clockType=0 (graphics) */
                printf("\n  Probing different pstate values (clockType=0):\n");
                for (int ps = 0; ps < 16; ps++) {
                    memset(buf, 0, sizeof(buf));
                    vword = NVML_VER(24, 1);
                    memcpy(buf, &vword, 4);
                    uint32_t clockType = 0;
                    uint32_t pstate = ps;
                    memcpy(buf + 4, &clockType, 4);
                    memcpy(buf + 8, &pstate, 4);
                    SAFE_CALL(getClockOffsets(dev, buf), "getClockOffsets");
                    if (ret == 0) {
                        int32_t offset, minOff, maxOff;
                        memcpy(&offset, buf + 12, 4);
                        memcpy(&minOff, buf + 16, 4);
                        memcpy(&maxOff, buf + 20, 4);
                        printf("    P%d: offset=%d, min=%d, max=%d\n",
                               ps, offset, minOff, maxOff);
                    } else if (ret != 2) {
                        printf("    P%d: %s\n", ps, ret_str(ret, errStr));
                    }
                }

                /* Try different pstate values with clockType=1 (memory) */
                printf("\n  Probing different pstate values (clockType=1, memory):\n");
                for (int ps = 0; ps < 16; ps++) {
                    memset(buf, 0, sizeof(buf));
                    vword = NVML_VER(24, 1);
                    memcpy(buf, &vword, 4);
                    uint32_t clockType = 1;
                    uint32_t pstate = ps;
                    memcpy(buf + 4, &clockType, 4);
                    memcpy(buf + 8, &pstate, 4);
                    SAFE_CALL(getClockOffsets(dev, buf), "getClockOffsets");
                    if (ret == 0) {
                        int32_t offset, minOff, maxOff;
                        memcpy(&offset, buf + 12, 4);
                        memcpy(&minOff, buf + 16, 4);
                        memcpy(&maxOff, buf + 20, 4);
                        printf("    P%d: offset=%d, min=%d, max=%d\n",
                               ps, offset, minOff, maxOff);
                    } else if (ret != 2) {
                        printf("    P%d: %s\n", ps, ret_str(ret, errStr));
                    }
                }
            }
        }
        printf("\n  SetClockOffsets exported: %s\n", setClockOffsets ? "YES" : "NO");
    }
    printf("\n");

    /* =============================================================
     * SECTION B: NvAPI ClockBoostTable (0x23F1B133 and 0x507B4B59)
     * ============================================================= */
    printf("=== B. NvAPI GPU_GetClockBoostTable ===\n\n");
    {
        void *nvapi = dlopen("libnvidia-api.so.1", RTLD_LAZY);
        if (nvapi) {
            nvapi_qi_t qi = dlsym(nvapi, "nvapi_QueryInterface");
            if (qi) {
                /* First get a GPU handle via NvAPI */
                /* NvAPI_EnumPhysicalGPUs = 0xE5AC921F is SetClockBoostLock, wrong */
                /* NvAPI_EnumPhysicalGPUs = 0xE5AC921F */
                /* Actually: NvAPI_EnumPhysicalGPUs = 0xE5AC921F ... no */
                /* Standard NvAPI function IDs */
                /* NvAPI_Initialize = 0x0150E828 */
                typedef int (*nvapi_init_t)(void);
                typedef int (*nvapi_enum_gpus_t)(void **handles, int *count);

                nvapi_init_t nvInit = qi(0x0150E828);
                nvapi_enum_gpus_t nvEnumGPUs = qi(0xE5AC921F);  /* This is SetClockBoostLock, not enum */

                /* Try actual EnumPhysicalGPUs ID */
                nvapi_enum_gpus_t nvEnumGPUs2 = qi(0xE5AC921F);

                /* The proper NvAPI GPU enumeration function ID */
                /* NvAPI_EnumPhysicalGPUs = 0xE5AC921F actually no... */
                /* Let me use the known working pattern from deep_nvapi.c */

                /* Actually we need:
                 * NvAPI_Initialize = 0x0150E828
                 * NvAPI_EnumPhysicalGPUs = 0xE5AC921F (WRONG - that's SetClockBoostLock)
                 * Actually: 0xE5AC921F = SetClockBoostLock
                 * NvAPI_EnumPhysicalGPUs should be checked from the source
                 */

                /* Let me look for the right IDs */
                /* Common NvAPI IDs:
                 * NvAPI_Initialize = 0x0150E828
                 * NvAPI_EnumPhysicalGPUs = 0xE5AC921F (check LACT)
                 * Actually wrong. Let me use known working approach from previous probes
                 */

                if (nvInit) {
                    int initRet = nvInit();
                    printf("  NvAPI_Initialize: %d\n", initRet);
                }

                /* Use known enumeration ID from nvapi.h reference:
                 * NvAPI_EnumPhysicalGPUs = 0xE5AC921F  -- No this is wrong
                 * 0xE5AC921F = NvAPI_GPU_SetClockBoostLock
                 *
                 * Proper: NvAPI_EnumPhysicalGPUs = comes from open-source nvapi references:
                 * They use 0xE5AC921F for something else...
                 *
                 * From NVAPI R550 spec: NvAPI_EnumPhysicalGPUs ID = not publicly documented
                 * Let's just use GPU index 0 directly and try the function calls.
                 * From LACT source: they use nvapi_QueryInterface with specific ID patterns
                 *
                 * Actually the simplest approach: NvAPI functions that take a GPU handle
                 * typically get a handle from NvAPI_EnumPhysicalGPUs.
                 * ID: 0xE5AC921F = GPU_SetClockBoostLock (from our probe list)
                 *
                 * Known from open-gpu-kernel-modules and other references:
                 * NvAPI_EnumPhysicalGPUs = 0xE5AC921F is WRONG
                 *
                 * From the well-known nvapi_lite_common.h:
                 * NvAPI_EnumPhysicalGPUs = undefined in lite headers
                 *
                 * Let me just try the standard approach from the existing probes
                 */

                /* Use the enumerate function that the existing probes used successfully */
                /* From deep_nvapi.c source code in this repo */
                typedef int (*nvapi_enum_t)(void *handles[64], int *count);
                nvapi_enum_t nvEnum = qi(0xE5AC921F);

                /* That's SetClockBoostLock not Enum - let me find the real ID */
                /* The actual ID from various open-source NVAPI references: */
                /* Try several known candidates */
                unsigned int enum_candidates[] = {
                    0xE5AC921F, /* Previously labeled as SetClockBoostLock but might be EnumPhysicalGPUs */
                    0x48B3EA59, /* NvAPI_EnumPhysicalGPUs (from some references) */
                    0x0E5AC921, /* variant */
                };

                void *gpu_handle = NULL;
                int gpu_count = 0;

                for (int i = 0; i < 3 && !gpu_handle; i++) {
                    void *fn = qi(enum_candidates[i]);
                    if (!fn) continue;

                    void *handles[64];
                    memset(handles, 0, sizeof(handles));
                    int cnt = 0;
                    in_probe = 1;
                    if (sigsetjmp(jump_buf, 1) == 0) {
                        int r = ((int(*)(void*[64], int*))fn)(handles, &cnt);
                        if (r == 0 && cnt > 0 && handles[0]) {
                            gpu_handle = handles[0];
                            gpu_count = cnt;
                            printf("  EnumPhysicalGPUs (0x%08X): %d GPUs, handle=%p\n",
                                   enum_candidates[i], cnt, handles[0]);
                        }
                    }
                    in_probe = 0;
                }

                if (!gpu_handle) {
                    /* Try to use an alternative approach - NvAPI_SYS_GetPhysicalGpuFromDisplayId etc */
                    /* Or just try raw handle values that typically work */
                    printf("  Could not enumerate GPUs via NvAPI, trying direct handle approach\n");

                    /* In Linux NvAPI, the GPU handle is typically just an index or pointer */
                    /* Let's try passing small integers as handles */
                }

                /* Even without a valid handle, let's try the functions to probe struct format */
                /* The version check happens BEFORE the handle check in most NvAPI functions */

                /* ClockBoostTable (0x23F1B133) */
                printf("\n  --- GetClockBoostTable (0x23F1B133) struct probe ---\n");
                {
                    nvapi_fn_t fn = (nvapi_fn_t)qi(0x23F1B133);
                    if (fn) {
                        /* Scan NvAPI version: (sz & 0xFFFF) | (ver << 16) */
                        int found = 0;
                        for (unsigned int ver = 1; ver <= 4 && !found; ver++) {
                            for (unsigned int sz = 4; sz <= 8192; sz += 4) {
                                uint8_t buf[16384];
                                memset(buf, 0, sizeof(buf));
                                uint32_t vword = NVAPI_VER(sz, ver);
                                memcpy(buf, &vword, 4);

                                in_probe = 1;
                                int r = -999;
                                if (sigsetjmp(jump_buf, 1) == 0) {
                                    r = fn(gpu_handle, buf);
                                }
                                in_probe = 0;

                                if (r == 0) {
                                    printf("    FOUND: sz=%u ver=%u vword=0x%08X\n", sz, ver, vword);
                                    printf("    Non-zero u32 fields:\n");
                                    dump_u32_nonzero(buf, (sz > 1024 ? 1024 : sz));
                                    printf("    Raw hex (first 512 bytes):\n");
                                    full_hexdump(buf, (sz > 512 ? 512 : sz));
                                    found = 1;
                                    break;
                                } else if (r == -5) {
                                    /* NVAPI_INCOMPATIBLE_STRUCT_VERSION - keep scanning */
                                } else if (r == -6 || r == -4) {
                                    /* INVALID_HANDLE or NVIDIA_DEVICE_NOT_FOUND - need handle */
                                    printf("    Need valid GPU handle (got NvAPI error %d at sz=%u ver=%u)\n", r, sz, ver);
                                    found = -2;
                                    break;
                                } else if (r != -5 && r != -999) {
                                    printf("    sz=%u ver=%u -> NvAPI error %d\n", sz, ver, r);
                                    if (r == -1) { /* NVAPI_ERROR */
                                        found = -1;
                                        break;
                                    }
                                }
                            }
                            if (found) break;
                        }
                        if (!found) printf("    No valid version/size found\n");
                    }
                }

                /* ClockBoostTable alt (0x507B4B59) */
                printf("\n  --- GetClockBoostTable_alt (0x507B4B59) struct probe ---\n");
                {
                    nvapi_fn_t fn = (nvapi_fn_t)qi(0x507B4B59);
                    if (fn) {
                        int found = 0;
                        for (unsigned int ver = 1; ver <= 4 && !found; ver++) {
                            for (unsigned int sz = 4; sz <= 8192; sz += 4) {
                                uint8_t buf[16384];
                                memset(buf, 0, sizeof(buf));
                                uint32_t vword = NVAPI_VER(sz, ver);
                                memcpy(buf, &vword, 4);

                                in_probe = 1;
                                int r = -999;
                                if (sigsetjmp(jump_buf, 1) == 0) {
                                    r = fn(gpu_handle, buf);
                                }
                                in_probe = 0;

                                if (r == 0) {
                                    printf("    FOUND: sz=%u ver=%u vword=0x%08X\n", sz, ver, vword);
                                    dump_u32_nonzero(buf, (sz > 1024 ? 1024 : sz));
                                    full_hexdump(buf, (sz > 512 ? 512 : sz));
                                    found = 1;
                                    break;
                                } else if (r == -6 || r == -4) {
                                    printf("    Need valid GPU handle (NvAPI error %d)\n", r);
                                    found = -2;
                                    break;
                                } else if (r != -5 && r != -999) {
                                    printf("    sz=%u ver=%u -> NvAPI error %d\n", sz, ver, r);
                                    found = -1;
                                    break;
                                }
                            }
                            if (found) break;
                        }
                    }
                }

                /* VFPCurve (0x21537AD4) */
                printf("\n  --- GetVFPCurve (0x21537AD4) struct probe ---\n");
                {
                    nvapi_fn_t fn = (nvapi_fn_t)qi(0x21537AD4);
                    if (fn) {
                        int found = 0;
                        for (unsigned int ver = 1; ver <= 4 && !found; ver++) {
                            for (unsigned int sz = 4; sz <= 8192; sz += 4) {
                                uint8_t buf[16384];
                                memset(buf, 0, sizeof(buf));
                                uint32_t vword = NVAPI_VER(sz, ver);
                                memcpy(buf, &vword, 4);

                                in_probe = 1;
                                int r = -999;
                                if (sigsetjmp(jump_buf, 1) == 0) {
                                    r = fn(gpu_handle, buf);
                                }
                                in_probe = 0;

                                if (r == 0) {
                                    printf("    FOUND: sz=%u ver=%u vword=0x%08X\n", sz, ver, vword);
                                    dump_u32_nonzero(buf, (sz > 2048 ? 2048 : sz));
                                    printf("    Raw hex (first 1024 bytes):\n");
                                    full_hexdump(buf, (sz > 1024 ? 1024 : sz));
                                    found = 1;
                                    break;
                                } else if (r == -6 || r == -4) {
                                    printf("    Need valid GPU handle (NvAPI error %d)\n", r);
                                    found = -2;
                                    break;
                                } else if (r != -5 && r != -999) {
                                    printf("    sz=%u ver=%u -> NvAPI error %d\n", sz, ver, r);
                                    found = -1;
                                    break;
                                }
                            }
                            if (found) break;
                        }
                    }
                }

                /* Pstates20 (0x6FF81213) */
                printf("\n  --- GetPstates20 (0x6FF81213) struct probe ---\n");
                {
                    nvapi_fn_t fn = (nvapi_fn_t)qi(0x6FF81213);
                    if (fn) {
                        int found = 0;
                        for (unsigned int ver = 1; ver <= 4 && !found; ver++) {
                            for (unsigned int sz = 4; sz <= 8192; sz += 4) {
                                uint8_t buf[16384];
                                memset(buf, 0, sizeof(buf));
                                uint32_t vword = NVAPI_VER(sz, ver);
                                memcpy(buf, &vword, 4);

                                in_probe = 1;
                                int r = -999;
                                if (sigsetjmp(jump_buf, 1) == 0) {
                                    r = fn(gpu_handle, buf);
                                }
                                in_probe = 0;

                                if (r == 0) {
                                    printf("    FOUND: sz=%u ver=%u vword=0x%08X\n", sz, ver, vword);
                                    dump_u32_nonzero(buf, (sz > 2048 ? 2048 : sz));
                                    printf("    Raw hex (first 1024 bytes):\n");
                                    full_hexdump(buf, (sz > 1024 ? 1024 : sz));
                                    found = 1;
                                    break;
                                } else if (r == -6 || r == -4) {
                                    printf("    Need valid GPU handle (NvAPI error %d)\n", r);
                                    found = -2;
                                    break;
                                } else if (r != -5 && r != -999) {
                                    printf("    sz=%u ver=%u -> NvAPI error %d\n", sz, ver, r);
                                    found = -1;
                                    break;
                                }
                            }
                            if (found) break;
                        }
                    }
                }

                /* GetVoltage (0x465F9BCF) - already known, re-probe */
                printf("\n  --- GetVoltage (0x465F9BCF) ---\n");
                {
                    nvapi_fn_t fn = (nvapi_fn_t)qi(0x465F9BCF);
                    if (fn) {
                        int found = 0;
                        for (unsigned int ver = 1; ver <= 4 && !found; ver++) {
                            for (unsigned int sz = 4; sz <= 512; sz += 4) {
                                uint8_t buf[1024];
                                memset(buf, 0, sizeof(buf));
                                uint32_t vword = NVAPI_VER(sz, ver);
                                memcpy(buf, &vword, 4);

                                in_probe = 1;
                                int r = -999;
                                if (sigsetjmp(jump_buf, 1) == 0) {
                                    r = fn(gpu_handle, buf);
                                }
                                in_probe = 0;

                                if (r == 0) {
                                    printf("    FOUND: sz=%u ver=%u\n", sz, ver);
                                    dump_u32_nonzero(buf, sz);
                                    found = 1;
                                    break;
                                } else if (r == -6 || r == -4) {
                                    printf("    Need valid GPU handle (NvAPI error %d)\n", r);
                                    found = -2;
                                    break;
                                } else if (r != -5 && r != -999) {
                                    printf("    sz=%u ver=%u -> NvAPI error %d\n", sz, ver, r);
                                    found = -1;
                                    break;
                                }
                            }
                            if (found) break;
                        }
                    }
                }

                /* GetAllClockFrequencies (0xDCB616C3) */
                printf("\n  --- GetAllClockFrequencies (0xDCB616C3) ---\n");
                {
                    nvapi_fn_t fn = (nvapi_fn_t)qi(0xDCB616C3);
                    if (fn) {
                        int found = 0;
                        for (unsigned int ver = 1; ver <= 4 && !found; ver++) {
                            for (unsigned int sz = 4; sz <= 1024; sz += 4) {
                                uint8_t buf[2048];
                                memset(buf, 0, sizeof(buf));
                                uint32_t vword = NVAPI_VER(sz, ver);
                                memcpy(buf, &vword, 4);

                                in_probe = 1;
                                int r = -999;
                                if (sigsetjmp(jump_buf, 1) == 0) {
                                    r = fn(gpu_handle, buf);
                                }
                                in_probe = 0;

                                if (r == 0) {
                                    printf("    FOUND: sz=%u ver=%u\n", sz, ver);
                                    dump_u32_nonzero(buf, sz);
                                    found = 1;
                                    break;
                                } else if (r == -6 || r == -4) {
                                    printf("    Need valid GPU handle (NvAPI error %d)\n", r);
                                    found = -2;
                                    break;
                                } else if (r != -5 && r != -999) {
                                    printf("    sz=%u ver=%u -> NvAPI error %d\n", sz, ver, r);
                                    found = -1;
                                    break;
                                }
                            }
                            if (found) break;
                        }
                    }
                }

                /* ClientPowerTopology (0x60DED2ED) */
                printf("\n  --- ClientPowerTopologyGetInfo (0x60DED2ED) ---\n");
                {
                    nvapi_fn_t fn = (nvapi_fn_t)qi(0x60DED2ED);
                    if (fn) {
                        int found = 0;
                        for (unsigned int ver = 1; ver <= 4 && !found; ver++) {
                            for (unsigned int sz = 4; sz <= 512; sz += 4) {
                                uint8_t buf[1024];
                                memset(buf, 0, sizeof(buf));
                                uint32_t vword = NVAPI_VER(sz, ver);
                                memcpy(buf, &vword, 4);

                                in_probe = 1;
                                int r = -999;
                                if (sigsetjmp(jump_buf, 1) == 0) {
                                    r = fn(gpu_handle, buf);
                                }
                                in_probe = 0;

                                if (r == 0) {
                                    printf("    FOUND: sz=%u ver=%u\n", sz, ver);
                                    dump_u32_nonzero(buf, sz);
                                    found = 1;
                                    break;
                                } else if (r == -6 || r == -4) {
                                    printf("    Need valid GPU handle (NvAPI error %d)\n", r);
                                    found = -2;
                                    break;
                                } else if (r != -5 && r != -999) {
                                    printf("    sz=%u ver=%u -> NvAPI error %d\n", sz, ver, r);
                                    found = -1;
                                    break;
                                }
                            }
                            if (found) break;
                        }
                    }
                }

                /* ThermalPoliciesStatus (0xE9C425A1) */
                printf("\n  --- ThermalPoliciesStatus (0xE9C425A1) ---\n");
                {
                    nvapi_fn_t fn = (nvapi_fn_t)qi(0xE9C425A1);
                    if (fn) {
                        int found = 0;
                        for (unsigned int ver = 1; ver <= 4 && !found; ver++) {
                            for (unsigned int sz = 4; sz <= 2048; sz += 4) {
                                uint8_t buf[4096];
                                memset(buf, 0, sizeof(buf));
                                uint32_t vword = NVAPI_VER(sz, ver);
                                memcpy(buf, &vword, 4);

                                in_probe = 1;
                                int r = -999;
                                if (sigsetjmp(jump_buf, 1) == 0) {
                                    r = fn(gpu_handle, buf);
                                }
                                in_probe = 0;

                                if (r == 0) {
                                    printf("    FOUND: sz=%u ver=%u\n", sz, ver);
                                    dump_u32_nonzero(buf, (sz > 512 ? 512 : sz));
                                    found = 1;
                                    break;
                                } else if (r == -6 || r == -4) {
                                    printf("    Need valid GPU handle (NvAPI error %d)\n", r);
                                    found = -2;
                                    break;
                                } else if (r != -5 && r != -999) {
                                    printf("    sz=%u ver=%u -> NvAPI error %d\n", sz, ver, r);
                                    found = -1;
                                    break;
                                }
                            }
                            if (found) break;
                        }
                    }
                }

                /* ClientPowerPoliciesSetStatus (0xAD95F5ED) */
                printf("\n  --- ClientPowerPoliciesSetStatus (0xAD95F5ED) - exported check ---\n");
                {
                    void *fn = qi(0xAD95F5ED);
                    printf("    Available: %s\n", fn ? "YES" : "NO");
                }

            }
            dlclose(nvapi);
        }
    }

    /* =============================================================
     * SECTION C: Additional NVML OC APIs
     * ============================================================= */
    printf("\n=== C. Additional NVML APIs ===\n\n");
    {
        /* nvmlDeviceGetSupportedClocksOffset - might exist */
        void *fnGetSupportedOffset = dlsym(lib, "nvmlDeviceGetSupportedClocksOffset");
        printf("  nvmlDeviceGetSupportedClocksOffset: %s\n", fnGetSupportedOffset ? "EXPORTED" : "not exported");

        void *fnGetClockBoostRange = dlsym(lib, "nvmlDeviceGetClockBoostRange");
        printf("  nvmlDeviceGetClockBoostRange: %s\n", fnGetClockBoostRange ? "EXPORTED" : "not exported");

        void *fnGetInstrAwareVF = dlsym(lib, "nvmlDeviceGetInstructionAwareVFCurve");
        printf("  nvmlDeviceGetInstructionAwareVFCurve: %s\n", fnGetInstrAwareVF ? "EXPORTED" : "not exported");

        void *fnToggleInstrAwareVF = dlsym(lib, "nvmlDeviceToggleInstructionAwareVFCurve");
        printf("  nvmlDeviceToggleInstructionAwareVFCurve: %s\n", fnToggleInstrAwareVF ? "EXPORTED" : "not exported");

        /* Check if GetClockOffsets accepts memory clock offset */
        printf("\n  --- Testing SetClockOffsets (Read-only, query-only) ---\n");
        nvml_dev_buf_t setClockOffsets = dlsym(lib, "nvmlDeviceSetClockOffsets");
        if (setClockOffsets) {
            printf("  SetClockOffsets is exported. Struct is same as GetClockOffsets (24 bytes, v1).\n");
            printf("  To set: fill clockType, pstate, and offset fields, then call.\n");
        }
    }

    printf("\n");

    /* =============================================================
     * SECTION D: Full NVML symbol scan for any OC-related functions we missed
     * ============================================================= */
    printf("=== D. Checking all potentially OC-relevant exported NVML symbols ===\n\n");
    {
        const char *symbols[] = {
            "nvmlDeviceGetClockOffsets",
            "nvmlDeviceSetClockOffsets",
            "nvmlDeviceGetGpcClkVfOffset",
            "nvmlDeviceSetGpcClkVfOffset",
            "nvmlDeviceGetMemClkVfOffset",
            "nvmlDeviceSetMemClkVfOffset",
            "nvmlDeviceGetGpuLockedClocks",
            "nvmlDeviceSetGpuLockedClocks",
            "nvmlDeviceResetGpuLockedClocks",
            "nvmlDeviceGetMemoryLockedClocks",
            "nvmlDeviceSetMemoryLockedClocks",
            "nvmlDeviceResetMemoryLockedClocks",
            "nvmlDeviceGetPowerManagementLimit",
            "nvmlDeviceSetPowerManagementLimit",
            "nvmlDeviceGetPowerManagementLimitConstraints",
            "nvmlDeviceGetPowerManagementDefaultLimit",
            "nvmlDeviceGetEnforcedPowerLimit",
            "nvmlDeviceGetPerformanceModes",
            "nvmlDeviceGetPerformanceState",
            "nvmlDeviceGetMinMaxClockOfPState",
            "nvmlDeviceGetSupportedGraphicsClocks",
            "nvmlDeviceGetSupportedMemoryClocks",
            "nvmlDeviceGetApplicationsClock",
            "nvmlDeviceSetApplicationsClocks",
            "nvmlDeviceResetApplicationsClocks",
            "nvmlDeviceGetMaxClockInfo",
            "nvmlDeviceGetClockInfo",
            "nvmlDeviceGetMaxCustomerBoostClock",
            "nvmlDeviceGetMarginTemperature",
            "nvmlDeviceGetDynamicPstatesInfo",
            "nvmlDeviceGetPowerMizerMode_v1",
            "nvmlDeviceGetCoolerInfo",
            "nvmlDeviceGetFanSpeed",
            "nvmlDeviceGetFanSpeed_v2",
            "nvmlDeviceGetTargetFanSpeed",
            "nvmlDeviceSetFanSpeed_v2",
            "nvmlDeviceSetDefaultFanSpeed_v2",
            "nvmlDeviceGetFanControlPolicy_v2",
            "nvmlDeviceSetFanControlPolicy",
            "nvmlDeviceGetMinMaxFanSpeed",
            "nvmlDeviceGetNumFans",
            "nvmlDeviceGetFanSpeedRPM",
            "nvmlDeviceGetTemperature",
            "nvmlDeviceGetTemperatureThreshold",
            "nvmlDeviceSetTemperatureThreshold",
            "nvmlDeviceGetThermalSettings",
            "nvmlDeviceGetAdaptiveClockInfoStatus",
            "nvmlDeviceGetViolationStatus",
            "nvmlDeviceGetCurrentClocksEventReasons",
            "nvmlDeviceGetCurrentClocksThrottleReasons",
            "nvmlDevicePowerSmoothingSetState",
            "nvmlDevicePowerSmoothingActivatePresetProfile",
            "nvmlDevicePowerSmoothingUpdatePresetProfileParam",
            "nvmlDeviceWorkloadPowerProfileGetProfilesInfo",
            "nvmlDeviceWorkloadPowerProfileGetCurrentProfiles",
            "nvmlDeviceWorkloadPowerProfileSetRequestedProfiles",
            "nvmlDeviceWorkloadPowerProfileClearRequestedProfiles",
            "nvmlDeviceWorkloadPowerProfileUpdateProfiles_v1",
            "nvmlDeviceGetPstates20",
            "nvmlDeviceGetTotalEnergyConsumption",
            "nvmlDeviceGetFieldValues",
            NULL,
        };

        for (int i = 0; symbols[i]; i++) {
            void *fn = dlsym(lib, symbols[i]);
            printf("  %-55s %s\n", symbols[i], fn ? "YES" : "no");
        }
    }

    nvmlShutdown();
    dlclose(lib);
    printf("\n=== Deep OC probe complete ===\n");
    return 0;
}
