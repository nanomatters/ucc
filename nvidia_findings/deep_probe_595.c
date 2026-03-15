/*
 * deep_probe_595.c - Comprehensive NVIDIA API probe for driver 595.45.04
 *
 * Probes all exported NVML functions relevant to overclocking, power management,
 * fan control, and thermal management on RTX 5090 (Blackwell, desktop).
 *
 * Build: gcc -O2 -o deep_probe_595 deep_probe_595.c -ldl
 * Run:   sudo ./deep_probe_595
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>
#include <signal.h>
#include <setjmp.h>

/* ====== NVML types ====== */
typedef void *nvmlDevice_t;
typedef int nvmlReturn_t;

#define NVML_SUCCESS 0
#define NVML_ERROR_NOT_SUPPORTED 3
#define NVML_ERROR_NO_PERMISSION 4
#define NVML_ERROR_INVALID_ARGUMENT 2
#define NVML_ERROR_NOT_FOUND 6
#define NVML_ERROR_FUNCTION_NOT_FOUND 13
#define NVML_ERROR_RESET_REQUIRED 16

/* Version word encoding: (struct_size & 0x00FFFFFF) | (version_number << 24) */
#define NVML_VER(sz, ver) (((sz) & 0x00FFFFFF) | ((ver) << 24))

/* ====== Function pointer types ====== */
typedef nvmlReturn_t (*nvmlInit_t)(void);
typedef nvmlReturn_t (*nvmlShutdown_t)(void);
typedef nvmlReturn_t (*nvmlDeviceGetHandleByIndex_t)(unsigned int, nvmlDevice_t *);
typedef nvmlReturn_t (*nvmlDeviceGetName_t)(nvmlDevice_t, char *, unsigned int);
typedef nvmlReturn_t (*nvmlDeviceGetCount_t)(unsigned int *);
typedef const char *(*nvmlErrorString_t)(nvmlReturn_t);

/* Generic versioned call: (device, void *buf) */
typedef nvmlReturn_t (*nvml_dev_buf_t)(nvmlDevice_t, void *);

/* ClockOffset get/set: (device, nvmlClockOffset_t *) */
typedef nvmlReturn_t (*nvml_clockoffset_get_t)(nvmlDevice_t, void *);
typedef nvmlReturn_t (*nvml_clockoffset_set_t)(nvmlDevice_t, void *);

/* VfOffset get/set: (device, int *) */
typedef nvmlReturn_t (*nvml_vfoffset_get_t)(nvmlDevice_t, int *);
typedef nvmlReturn_t (*nvml_vfoffset_set_t)(nvmlDevice_t, int);

/* GetMinMaxClockOfPState: (device, clockType, pstate, uint*, uint*) */
typedef nvmlReturn_t (*nvml_minmax_pstate_t)(nvmlDevice_t, unsigned int, unsigned int, unsigned int *, unsigned int *);

/* Power limit: (device, uint) */
typedef nvmlReturn_t (*nvml_power_get_t)(nvmlDevice_t, unsigned int *);
typedef nvmlReturn_t (*nvml_power_set_t)(nvmlDevice_t, unsigned int);

/* Locked clocks: (device, uint, uint) */
typedef nvmlReturn_t (*nvml_locked_clocks_t)(nvmlDevice_t, unsigned int, unsigned int);
typedef nvmlReturn_t (*nvml_reset_locked_t)(nvmlDevice_t);

/* Fan: (device, uint fan, uint *speed) */
typedef nvmlReturn_t (*nvml_fan_speed_t)(nvmlDevice_t, unsigned int, unsigned int *);
typedef nvmlReturn_t (*nvml_fan_set_t)(nvmlDevice_t, unsigned int, unsigned int);
typedef nvmlReturn_t (*nvml_fan_policy_get_t)(nvmlDevice_t, unsigned int, unsigned int *);
typedef nvmlReturn_t (*nvml_fan_policy_set_t)(nvmlDevice_t, unsigned int, unsigned int);

/* Simple (device, uint*) */
typedef nvmlReturn_t (*nvml_dev_uint_t)(nvmlDevice_t, unsigned int *);

/* ViolationStatus: (device, perfPolicyType, violationTime_t*) */
typedef nvmlReturn_t (*nvml_violation_t)(nvmlDevice_t, unsigned int, void *);

/* ====== Crash protection ====== */
static sigjmp_buf jump_buf;
static volatile int in_probe = 0;

static void crash_handler(int sig) {
    if (in_probe) {
        siglongjmp(jump_buf, sig);
    }
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

static const char *ret_str(nvmlReturn_t ret, nvmlErrorString_t errStr) {
    if (ret == -999) return "CRASHED";
    if (ret == 0) return "SUCCESS";
    if (errStr) return errStr(ret);
    static char buf[32];
    snprintf(buf, sizeof(buf), "error=%d", ret);
    return buf;
}

static void hexdump(const void *data, size_t len, size_t max) {
    const uint8_t *p = (const uint8_t *)data;
    if (len > max) len = max;
    for (size_t i = 0; i < len; i++) {
        if (i > 0 && i % 32 == 0) printf("\n    ");
        printf("%02x ", p[i]);
    }
    printf("\n");
}

static void dump_u32_fields(const void *data, size_t len, const char *prefix) {
    const uint32_t *p = (const uint32_t *)data;
    size_t count = len / 4;
    if (count > 64) count = 64;
    for (size_t i = 0; i < count; i++) {
        if (p[i] != 0) {
            printf("  %s[%zu] = 0x%08X (%u / %d)\n", prefix, i, p[i], p[i], (int32_t)p[i]);
        }
    }
}

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = crash_handler;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);

    void *lib = dlopen("libnvidia-ml.so.1", RTLD_LAZY);
    if (!lib) {
        fprintf(stderr, "Failed to load libnvidia-ml.so.1: %s\n", dlerror());
        return 1;
    }

    nvmlInit_t nvmlInit = dlsym(lib, "nvmlInit_v2");
    nvmlShutdown_t nvmlShutdown = dlsym(lib, "nvmlShutdown");
    nvmlDeviceGetHandleByIndex_t getHandle = dlsym(lib, "nvmlDeviceGetHandleByIndex_v2");
    nvmlDeviceGetName_t getName = dlsym(lib, "nvmlDeviceGetName");
    nvmlDeviceGetCount_t getCount = dlsym(lib, "nvmlDeviceGetCount_v2");
    nvmlErrorString_t errStr = dlsym(lib, "nvmlErrorString");

    nvmlReturn_t ret;
    ret = nvmlInit();
    if (ret != NVML_SUCCESS) {
        fprintf(stderr, "nvmlInit failed: %d\n", ret);
        return 1;
    }

    unsigned int count = 0;
    getCount(&count);
    printf("=== NVIDIA Driver 595.45.04 Deep Probe ===\n");
    printf("GPU count: %u\n\n", count);

    for (unsigned int idx = 0; idx < count; idx++) {
        nvmlDevice_t dev;
        ret = getHandle(idx, &dev);
        if (ret != NVML_SUCCESS) {
            printf("Failed to get handle for GPU %u: %s\n", idx, ret_str(ret, errStr));
            continue;
        }

        char name[256] = {0};
        getName(dev, name, sizeof(name));
        printf("========================================\n");
        printf("GPU %u: %s\n", idx, name);
        printf("========================================\n\n");

        /* -----------------------------------------------------------
         * 1. CLOCK OFFSET APIs (NEW - nvmlClockOffset_t struct)
         * ----------------------------------------------------------- */
        printf("--- 1. nvmlDeviceGetClockOffsets ---\n");
        {
            nvml_clockoffset_get_t getClockOffsets = dlsym(lib, "nvmlDeviceGetClockOffsets");
            if (getClockOffsets) {
                /* Try different version/size combinations */
                /* The signature string says: (nvmlDevice_t device, nvmlClockOffset_t *info) */
                /* Let's probe sizes from 8 to 256 in steps */
                int found = 0;
                for (unsigned int ver = 1; ver <= 3 && !found; ver++) {
                    for (unsigned int sz = 8; sz <= 256; sz += 4) {
                        uint8_t buf[512];
                        memset(buf, 0, sizeof(buf));
                        uint32_t vword = NVML_VER(sz, ver);
                        memcpy(buf, &vword, 4);
                        SAFE_CALL(getClockOffsets(dev, buf), "getClockOffsets");
                        if (ret == NVML_SUCCESS) {
                            printf("  FOUND: sz=%u, ver=%u, vword=0x%08X\n", sz, ver, vword);
                            dump_u32_fields(buf, sz, "f");
                            printf("  Raw hex (first 128 bytes):\n    ");
                            hexdump(buf, sz, 128);
                            found = 1;
                            break;
                        } else if (ret != NVML_ERROR_INVALID_ARGUMENT && ret != -999) {
                            printf("  sz=%u ver=%u -> %s\n", sz, ver, ret_str(ret, errStr));
                            if (ret == NVML_ERROR_NOT_SUPPORTED) {
                                found = -1;
                                break;
                            }
                        }
                    }
                    if (found == -1) break;
                }
                if (!found) printf("  No valid version/size combination found\n");
                if (found == -1) printf("  API returned NOT_SUPPORTED\n");
            } else {
                printf("  NOT EXPORTED\n");
            }
        }
        printf("\n");

        /* -----------------------------------------------------------
         * 2. nvmlDeviceSetClockOffsets (just check existence, don't call SET)
         * ----------------------------------------------------------- */
        printf("--- 2. nvmlDeviceSetClockOffsets ---\n");
        {
            void *fn = dlsym(lib, "nvmlDeviceSetClockOffsets");
            printf("  Exported: %s\n", fn ? "YES" : "NO");
            /* We'll probe the struct layout using Get first, then document Set */
        }
        printf("\n");

        /* -----------------------------------------------------------
         * 3. GetMinMaxClockOfPState (documented signature in strings)
         * ----------------------------------------------------------- */
        printf("--- 3. nvmlDeviceGetMinMaxClockOfPState ---\n");
        {
            nvml_minmax_pstate_t getMinMax = dlsym(lib, "nvmlDeviceGetMinMaxClockOfPState");
            if (getMinMax) {
                /* clockType: 0=graphics, 1=SM, 2=mem, 3=video */
                /* pstate: 0-15 */
                const char *clockNames[] = {"Graphics", "SM", "Memory", "Video"};
                for (unsigned int ct = 0; ct <= 3; ct++) {
                    for (unsigned int ps = 0; ps <= 4; ps++) {
                        unsigned int minMHz = 0, maxMHz = 0;
                        SAFE_CALL(getMinMax(dev, ct, ps, &minMHz, &maxMHz), "getMinMax");
                        if (ret == NVML_SUCCESS) {
                            printf("  %s P%u: min=%u MHz, max=%u MHz\n", clockNames[ct], ps, minMHz, maxMHz);
                        } else if (ret != NVML_ERROR_NOT_SUPPORTED && ret != NVML_ERROR_INVALID_ARGUMENT) {
                            printf("  %s P%u: %s\n", clockNames[ct], ps, ret_str(ret, errStr));
                        }
                    }
                }
            } else {
                printf("  NOT EXPORTED\n");
            }
        }
        printf("\n");

        /* -----------------------------------------------------------
         * 4. VfOffset APIs (existing, test on new driver)
         * ----------------------------------------------------------- */
        printf("--- 4. VfOffset APIs ---\n");
        {
            nvml_vfoffset_get_t getGpcVf = dlsym(lib, "nvmlDeviceGetGpcClkVfOffset");
            nvml_vfoffset_get_t getMemVf = dlsym(lib, "nvmlDeviceGetMemClkVfOffset");
            if (getGpcVf) {
                int off = 0;
                SAFE_CALL(getGpcVf(dev, &off), "getGpcVf");
                printf("  GPC VfOffset: %s -> %d\n", ret_str(ret, errStr), off);
            } else printf("  nvmlDeviceGetGpcClkVfOffset: NOT EXPORTED\n");

            if (getMemVf) {
                int off = 0;
                SAFE_CALL(getMemVf(dev, &off), "getMemVf");
                printf("  Mem VfOffset: %s -> %d\n", ret_str(ret, errStr), off);
            } else printf("  nvmlDeviceGetMemClkVfOffset: NOT EXPORTED\n");
        }
        printf("\n");

        /* -----------------------------------------------------------
         * 5. Power Smoothing APIs (NEW)
         * ----------------------------------------------------------- */
        printf("--- 5. Power Smoothing APIs ---\n");
        {
            nvml_dev_buf_t pwrSmoothSetState = dlsym(lib, "nvmlDevicePowerSmoothingSetState");
            nvml_dev_buf_t pwrSmoothActivate = dlsym(lib, "nvmlDevicePowerSmoothingActivatePresetProfile");
            nvml_dev_buf_t pwrSmoothUpdate = dlsym(lib, "nvmlDevicePowerSmoothingUpdatePresetProfileParam");

            printf("  SetState exported: %s\n", pwrSmoothSetState ? "YES" : "NO");
            printf("  ActivatePresetProfile exported: %s\n", pwrSmoothActivate ? "YES" : "NO");
            printf("  UpdatePresetProfileParam exported: %s\n", pwrSmoothUpdate ? "YES" : "NO");

            /* Probe PowerSmoothingSetState with version scanning */
            if (pwrSmoothSetState) {
                printf("\n  Probing PowerSmoothingSetState:\n");
                int found = 0;
                for (unsigned int ver = 1; ver <= 3 && !found; ver++) {
                    for (unsigned int sz = 8; sz <= 512; sz += 4) {
                        uint8_t buf[1024];
                        memset(buf, 0, sizeof(buf));
                        uint32_t vword = NVML_VER(sz, ver);
                        memcpy(buf, &vword, 4);
                        SAFE_CALL(pwrSmoothSetState(dev, buf), "pwrSmoothSetState");
                        if (ret == NVML_SUCCESS || ret == NVML_ERROR_NOT_SUPPORTED) {
                            printf("    sz=%u, ver=%u, vword=0x%08X -> %s\n", sz, ver, vword,
                                   ret_str(ret, errStr));
                            if (ret == NVML_SUCCESS) {
                                dump_u32_fields(buf, sz, "f");
                                found = 1;
                            } else {
                                found = -1;
                            }
                            break;
                        }
                    }
                    if (found) break;
                }
                if (!found) printf("    No valid version found\n");
            }

            /* Probe ActivatePresetProfile */
            if (pwrSmoothActivate) {
                printf("\n  Probing ActivatePresetProfile:\n");
                int found = 0;
                for (unsigned int ver = 1; ver <= 3 && !found; ver++) {
                    for (unsigned int sz = 8; sz <= 512; sz += 4) {
                        uint8_t buf[1024];
                        memset(buf, 0, sizeof(buf));
                        uint32_t vword = NVML_VER(sz, ver);
                        memcpy(buf, &vword, 4);
                        SAFE_CALL(pwrSmoothActivate(dev, buf), "pwrSmoothActivate");
                        if (ret == NVML_SUCCESS || ret == NVML_ERROR_NOT_SUPPORTED) {
                            printf("    sz=%u, ver=%u, vword=0x%08X -> %s\n", sz, ver, vword,
                                   ret_str(ret, errStr));
                            if (ret == NVML_SUCCESS) {
                                dump_u32_fields(buf, sz, "f");
                                found = 1;
                            } else {
                                found = -1;
                            }
                            break;
                        }
                    }
                    if (found) break;
                }
                if (!found) printf("    No valid version found\n");
            }

            /* Probe UpdatePresetProfileParam */
            if (pwrSmoothUpdate) {
                printf("\n  Probing UpdatePresetProfileParam:\n");
                int found = 0;
                for (unsigned int ver = 1; ver <= 3 && !found; ver++) {
                    for (unsigned int sz = 8; sz <= 512; sz += 4) {
                        uint8_t buf[1024];
                        memset(buf, 0, sizeof(buf));
                        uint32_t vword = NVML_VER(sz, ver);
                        memcpy(buf, &vword, 4);
                        SAFE_CALL(pwrSmoothUpdate(dev, buf), "pwrSmoothUpdate");
                        if (ret == NVML_SUCCESS || ret == NVML_ERROR_NOT_SUPPORTED) {
                            printf("    sz=%u, ver=%u, vword=0x%08X -> %s\n", sz, ver, vword,
                                   ret_str(ret, errStr));
                            if (ret == NVML_SUCCESS) {
                                dump_u32_fields(buf, sz, "f");
                                found = 1;
                            } else {
                                found = -1;
                            }
                            break;
                        }
                    }
                    if (found) break;
                }
                if (!found) printf("    No valid version found\n");
            }
        }
        printf("\n");

        /* -----------------------------------------------------------
         * 6. Workload Power Profile APIs (NEW)
         * ----------------------------------------------------------- */
        printf("--- 6. Workload Power Profile APIs ---\n");
        {
            nvml_dev_buf_t getProfiles = dlsym(lib, "nvmlDeviceWorkloadPowerProfileGetProfilesInfo");
            nvml_dev_buf_t getCurrent = dlsym(lib, "nvmlDeviceWorkloadPowerProfileGetCurrentProfiles");
            nvml_dev_buf_t setRequested = dlsym(lib, "nvmlDeviceWorkloadPowerProfileSetRequestedProfiles");
            nvml_dev_buf_t clearRequested = dlsym(lib, "nvmlDeviceWorkloadPowerProfileClearRequestedProfiles");
            nvml_dev_buf_t updateV1 = dlsym(lib, "nvmlDeviceWorkloadPowerProfileUpdateProfiles_v1");

            printf("  GetProfilesInfo: %s\n", getProfiles ? "YES" : "NO");
            printf("  GetCurrentProfiles: %s\n", getCurrent ? "YES" : "NO");
            printf("  SetRequestedProfiles: %s\n", setRequested ? "YES" : "NO");
            printf("  ClearRequestedProfiles: %s\n", clearRequested ? "YES" : "NO");
            printf("  UpdateProfiles_v1: %s\n", updateV1 ? "YES" : "NO");

            /* Probe GetProfilesInfo */
            if (getProfiles) {
                printf("\n  Probing GetProfilesInfo:\n");
                int found = 0;
                for (unsigned int ver = 1; ver <= 3 && !found; ver++) {
                    for (unsigned int sz = 8; sz <= 2048; sz += 4) {
                        uint8_t buf[4096];
                        memset(buf, 0, sizeof(buf));
                        uint32_t vword = NVML_VER(sz, ver);
                        memcpy(buf, &vword, 4);
                        SAFE_CALL(getProfiles(dev, buf), "getProfilesInfo");
                        if (ret == NVML_SUCCESS) {
                            printf("    FOUND: sz=%u, ver=%u, vword=0x%08X\n", sz, ver, vword);
                            dump_u32_fields(buf, sz, "f");
                            printf("    Raw hex (first 256 bytes):\n    ");
                            hexdump(buf, sz, 256);
                            found = 1;
                            break;
                        } else if (ret == NVML_ERROR_NOT_SUPPORTED) {
                            printf("    sz=%u ver=%u -> NOT_SUPPORTED\n", sz, ver);
                            found = -1;
                            break;
                        }
                    }
                    if (found) break;
                }
            }

            /* Probe GetCurrentProfiles */
            if (getCurrent) {
                printf("\n  Probing GetCurrentProfiles:\n");
                int found = 0;
                for (unsigned int ver = 1; ver <= 3 && !found; ver++) {
                    for (unsigned int sz = 8; sz <= 2048; sz += 4) {
                        uint8_t buf[4096];
                        memset(buf, 0, sizeof(buf));
                        uint32_t vword = NVML_VER(sz, ver);
                        memcpy(buf, &vword, 4);
                        SAFE_CALL(getCurrent(dev, buf), "getCurrentProfiles");
                        if (ret == NVML_SUCCESS) {
                            printf("    FOUND: sz=%u, ver=%u, vword=0x%08X\n", sz, ver, vword);
                            dump_u32_fields(buf, sz, "f");
                            printf("    Raw hex (first 256 bytes):\n    ");
                            hexdump(buf, sz, 256);
                            found = 1;
                            break;
                        } else if (ret == NVML_ERROR_NOT_SUPPORTED) {
                            printf("    sz=%u ver=%u -> NOT_SUPPORTED\n", sz, ver);
                            found = -1;
                            break;
                        }
                    }
                    if (found) break;
                }
            }
        }
        printf("\n");

        /* -----------------------------------------------------------
         * 7. Cooler/Fan APIs
         * ----------------------------------------------------------- */
        printf("--- 7. Fan & Cooler APIs ---\n");
        {
            /* Standard fan APIs */
            nvml_dev_uint_t getNumFans = dlsym(lib, "nvmlDeviceGetNumFans");
            nvml_fan_speed_t getFanSpeedV2 = dlsym(lib, "nvmlDeviceGetFanSpeed_v2");
            nvml_fan_speed_t getTargetFan = dlsym(lib, "nvmlDeviceGetTargetFanSpeed");
            nvml_fan_speed_t getFanRPM = dlsym(lib, "nvmlDeviceGetFanSpeedRPM");
            nvml_fan_policy_get_t getFanPolicy = dlsym(lib, "nvmlDeviceGetFanControlPolicy_v2");

            unsigned int numFans = 0;
            if (getNumFans) {
                SAFE_CALL(getNumFans(dev, &numFans), "getNumFans");
                printf("  Fan count: %u (%s)\n", numFans, ret_str(ret, errStr));
            }

            for (unsigned int f = 0; f < (numFans > 0 ? numFans : 2); f++) {
                printf("  Fan %u:\n", f);
                if (getFanSpeedV2) {
                    unsigned int speed = 0;
                    SAFE_CALL(getFanSpeedV2(dev, f, &speed), "getFanSpeedV2");
                    printf("    Speed: %u%% (%s)\n", speed, ret_str(ret, errStr));
                }
                if (getTargetFan) {
                    unsigned int target = 0;
                    SAFE_CALL(getTargetFan(dev, f, &target), "getTargetFan");
                    printf("    Target: %u%% (%s)\n", target, ret_str(ret, errStr));
                }
                if (getFanRPM) {
                    unsigned int rpm = 0;
                    SAFE_CALL(getFanRPM(dev, f, &rpm), "getFanRPM");
                    printf("    RPM: %u (%s)\n", rpm, ret_str(ret, errStr));
                }
                if (getFanPolicy) {
                    unsigned int policy = 0;
                    SAFE_CALL(getFanPolicy(dev, f, &policy), "getFanPolicy");
                    printf("    Policy: %u (%s)\n", policy, ret_str(ret, errStr));
                }
            }

            /* MinMax fan speed */
            typedef nvmlReturn_t (*nvml_minmax_fan_t)(nvmlDevice_t, unsigned int *, unsigned int *);
            nvml_minmax_fan_t getMinMaxFan = dlsym(lib, "nvmlDeviceGetMinMaxFanSpeed");
            if (getMinMaxFan) {
                unsigned int minS = 0, maxS = 0;
                SAFE_CALL(getMinMaxFan(dev, &minS, &maxS), "getMinMaxFan");
                printf("  MinMax fan speed: min=%u%%, max=%u%% (%s)\n", minS, maxS, ret_str(ret, errStr));
            }

            /* Cooler info (versioned struct) */
            printf("\n  CoolerInfo probe:\n");
            nvml_dev_buf_t getCoolerInfo = dlsym(lib, "nvmlDeviceGetCoolerInfo");
            if (getCoolerInfo) {
                int found = 0;
                for (unsigned int ver = 1; ver <= 3 && !found; ver++) {
                    for (unsigned int sz = 8; sz <= 256; sz += 4) {
                        uint8_t buf[512];
                        memset(buf, 0, sizeof(buf));
                        uint32_t vword = NVML_VER(sz, ver);
                        memcpy(buf, &vword, 4);
                        SAFE_CALL(getCoolerInfo(dev, buf), "getCoolerInfo");
                        if (ret == NVML_SUCCESS) {
                            printf("    FOUND: sz=%u, ver=%u, vword=0x%08X\n", sz, ver, vword);
                            dump_u32_fields(buf, sz, "f");
                            found = 1;
                            break;
                        } else if (ret == NVML_ERROR_NOT_SUPPORTED) {
                            printf("    sz=%u ver=%u -> NOT_SUPPORTED\n", sz, ver);
                            found = -1;
                            break;
                        }
                    }
                    if (found) break;
                }
            } else {
                printf("    NOT EXPORTED\n");
            }
        }
        printf("\n");

        /* -----------------------------------------------------------
         * 8. PerformanceModes (known working, verify on new driver)
         * ----------------------------------------------------------- */
        printf("--- 8. PerformanceModes ---\n");
        {
            nvml_dev_buf_t getPerfModes = dlsym(lib, "nvmlDeviceGetPerformanceModes");
            if (getPerfModes) {
                uint8_t buf[4096];
                memset(buf, 0, sizeof(buf));
                uint32_t vword = NVML_VER(2052, 1);
                memcpy(buf, &vword, 4);
                SAFE_CALL(getPerfModes(dev, buf), "getPerfModes");
                printf("  Status: %s\n", ret_str(ret, errStr));
                if (ret == NVML_SUCCESS) {
                    printf("  Text: %.1024s\n", (char *)(buf + 4));
                }
            } else {
                printf("  NOT EXPORTED\n");
            }
        }
        printf("\n");

        /* -----------------------------------------------------------
         * 9. MarginTemperature (known working, verify on new driver)
         * ----------------------------------------------------------- */
        printf("--- 9. MarginTemperature ---\n");
        {
            nvml_dev_buf_t getMargin = dlsym(lib, "nvmlDeviceGetMarginTemperature");
            if (getMargin) {
                uint8_t buf[32];
                memset(buf, 0, sizeof(buf));
                uint32_t vword = NVML_VER(8, 1);
                memcpy(buf, &vword, 4);
                SAFE_CALL(getMargin(dev, buf), "getMargin");
                printf("  Status: %s\n", ret_str(ret, errStr));
                if (ret == NVML_SUCCESS) {
                    uint32_t margin;
                    memcpy(&margin, buf + 4, 4);
                    printf("  Thermal margin: %u°C\n", margin);
                }
            }
        }
        printf("\n");

        /* -----------------------------------------------------------
         * 10. DynamicPstatesInfo (verify on new driver)
         * ----------------------------------------------------------- */
        printf("--- 10. DynamicPstatesInfo ---\n");
        {
            nvml_dev_buf_t getDynPstates = dlsym(lib, "nvmlDeviceGetDynamicPstatesInfo");
            if (getDynPstates) {
                uint8_t buf[128];
                memset(buf, 0, sizeof(buf));
                SAFE_CALL(getDynPstates(dev, buf), "getDynPstates");
                printf("  Status: %s\n", ret_str(ret, errStr));
                if (ret == NVML_SUCCESS) {
                    uint32_t flags;
                    memcpy(&flags, buf, 4);
                    printf("  Flags: %u\n", flags);
                    const char *slotNames[] = {"GPU", "FrameBuffer", "VideoEngine", "Bus", "Slot4", "Slot5", "Slot6", "Slot7"};
                    for (int s = 0; s < 8; s++) {
                        uint32_t present, pct;
                        memcpy(&present, buf + 4 + s * 8, 4);
                        memcpy(&pct, buf + 8 + s * 8, 4);
                        if (present)
                            printf("  %s: %u%%\n", slotNames[s], pct);
                    }
                }
            }
        }
        printf("\n");

        /* -----------------------------------------------------------
         * 11. Power Management 
         * ----------------------------------------------------------- */
        printf("--- 11. Power Management ---\n");
        {
            nvml_power_get_t getPowerLimit = dlsym(lib, "nvmlDeviceGetPowerManagementLimit");
            nvml_power_get_t getDefPowerLimit = dlsym(lib, "nvmlDeviceGetPowerManagementDefaultLimit");
            typedef nvmlReturn_t (*nvml_power_constraint_t)(nvmlDevice_t, unsigned int *, unsigned int *);
            nvml_power_constraint_t getPowerConstraints = dlsym(lib, "nvmlDeviceGetPowerManagementLimitConstraints");
            nvml_power_get_t getEnfPowerLimit = dlsym(lib, "nvmlDeviceGetEnforcedPowerLimit");
            typedef nvmlReturn_t (*nvml_energy_t)(nvmlDevice_t, unsigned long long *);
            nvml_energy_t getEnergy = dlsym(lib, "nvmlDeviceGetTotalEnergyConsumption");
            nvml_power_get_t getPowerUsage = dlsym(lib, "nvmlDeviceGetPowerUsage");

            if (getPowerLimit) {
                unsigned int limit = 0;
                SAFE_CALL(getPowerLimit(dev, &limit), "getPowerLimit");
                printf("  Current power limit: %u mW (%u W) [%s]\n", limit, limit/1000, ret_str(ret, errStr));
            }
            if (getDefPowerLimit) {
                unsigned int def = 0;
                SAFE_CALL(getDefPowerLimit(dev, &def), "getDefPowerLimit");
                printf("  Default power limit: %u mW (%u W) [%s]\n", def, def/1000, ret_str(ret, errStr));
            }
            if (getPowerConstraints) {
                unsigned int minP = 0, maxP = 0;
                SAFE_CALL(getPowerConstraints(dev, &minP, &maxP), "getPowerConstraints");
                printf("  Power constraints: min=%u mW (%u W), max=%u mW (%u W) [%s]\n",
                       minP, minP/1000, maxP, maxP/1000, ret_str(ret, errStr));
            }
            if (getEnfPowerLimit) {
                unsigned int enf = 0;
                SAFE_CALL(getEnfPowerLimit(dev, &enf), "getEnfPowerLimit");
                printf("  Enforced power limit: %u mW (%u W) [%s]\n", enf, enf/1000, ret_str(ret, errStr));
            }
            if (getPowerUsage) {
                unsigned int usage = 0;
                SAFE_CALL(getPowerUsage(dev, &usage), "getPowerUsage");
                printf("  Current power usage: %u mW (%.1f W) [%s]\n", usage, usage/1000.0, ret_str(ret, errStr));
            }
            if (getEnergy) {
                unsigned long long energy = 0;
                SAFE_CALL(getEnergy(dev, &energy), "getEnergy");
                printf("  Total energy: %llu mJ [%s]\n", energy, ret_str(ret, errStr));
            }
        }
        printf("\n");

        /* -----------------------------------------------------------
         * 12. Locked Clocks APIs
         * ----------------------------------------------------------- */
        printf("--- 12. Locked Clocks ---\n");
        {
            /* Check deferred lock clocks */
            nvml_dev_buf_t supportsDeferred = dlsym(lib, "nvmlDeviceSupportsLockClocksDeferred");
            printf("  nvmlDeviceSupportsLockClocksDeferred: %s\n", supportsDeferred ? "EXPORTED" : "NOT EXPORTED");

            /* Get current GPU clock info */
            typedef nvmlReturn_t (*nvml_clock_get_t)(nvmlDevice_t, unsigned int, unsigned int *);
            nvml_clock_get_t getClockInfo = dlsym(lib, "nvmlDeviceGetClockInfo");
            if (getClockInfo) {
                const char *ctypes[] = {"Graphics", "SM", "Memory", "Video"};
                for (int ct = 0; ct < 4; ct++) {
                    unsigned int clk = 0;
                    SAFE_CALL(getClockInfo(dev, ct, &clk), "getClockInfo");
                    printf("  Current %s clock: %u MHz [%s]\n", ctypes[ct], clk, ret_str(ret, errStr));
                }
            }

            /* Max clocks */
            nvml_clock_get_t getMaxClock = dlsym(lib, "nvmlDeviceGetMaxClockInfo");
            if (getMaxClock) {
                const char *ctypes[] = {"Graphics", "SM", "Memory", "Video"};
                for (int ct = 0; ct < 4; ct++) {
                    unsigned int clk = 0;
                    SAFE_CALL(getMaxClock(dev, ct, &clk), "getMaxClock");
                    printf("  Max %s clock: %u MHz [%s]\n", ctypes[ct], clk, ret_str(ret, errStr));
                }
            }

            /* Max customer boost clock */
            nvml_clock_get_t getMaxBoost = dlsym(lib, "nvmlDeviceGetMaxCustomerBoostClock");
            if (getMaxBoost) {
                unsigned int clk = 0;
                SAFE_CALL(getMaxBoost(dev, 0, &clk), "getMaxBoost-graphics");
                printf("  Max customer boost (graphics): %u MHz [%s]\n", clk, ret_str(ret, errStr));
                SAFE_CALL(getMaxBoost(dev, 2, &clk), "getMaxBoost-mem");
                printf("  Max customer boost (memory): %u MHz [%s]\n", clk, ret_str(ret, errStr));
            }
        }
        printf("\n");

        /* -----------------------------------------------------------
         * 13. Violation Status (throttle reasons)
         * ----------------------------------------------------------- */
        printf("--- 13. Violation & Throttle Status ---\n");
        {
            nvml_violation_t getViolation = dlsym(lib, "nvmlDeviceGetViolationStatus");
            if (getViolation) {
                /* perfPolicyType: 0=Power, 1=Thermal, 2=SyncBoost, 3=BoardLimit, 4=LowUtil,
                   5=Reliability, 6=AppClockSetting, 7=BaseClockSetting */
                const char *types[] = {"Power", "Thermal", "SyncBoost", "BoardLimit",
                                       "LowUtil", "Reliability", "AppClock", "BaseClock"};
                for (int t = 0; t < 8; t++) {
                    struct { uint64_t ref; uint64_t violation; } times = {0, 0};
                    SAFE_CALL(getViolation(dev, t, &times), "getViolation");
                    if (ret == NVML_SUCCESS) {
                        printf("  %s: ref=%llu ns, violation=%llu ns\n",
                               types[t], (unsigned long long)times.ref, (unsigned long long)times.violation);
                    } else if (ret != NVML_ERROR_NOT_SUPPORTED) {
                        printf("  %s: %s\n", types[t], ret_str(ret, errStr));
                    }
                }
            }

            /* Throttle reasons */
            typedef nvmlReturn_t (*nvml_throttle_t)(nvmlDevice_t, unsigned long long *);
            nvml_throttle_t getThrottle = dlsym(lib, "nvmlDeviceGetCurrentClocksEventReasons");
            if (!getThrottle)
                getThrottle = dlsym(lib, "nvmlDeviceGetCurrentClocksThrottleReasons");
            if (getThrottle) {
                unsigned long long reasons = 0;
                SAFE_CALL(getThrottle(dev, &reasons), "getThrottle");
                printf("  Throttle reasons: 0x%016llX [%s]\n", reasons, ret_str(ret, errStr));
                if (ret == NVML_SUCCESS && reasons) {
                    if (reasons & 0x01) printf("    - GPU Idle\n");
                    if (reasons & 0x02) printf("    - Apps Clocks Setting\n");
                    if (reasons & 0x04) printf("    - SW Power Cap\n");
                    if (reasons & 0x08) printf("    - HW Slowdown\n");
                    if (reasons & 0x10) printf("    - Sync Boost\n");
                    if (reasons & 0x20) printf("    - SW Thermal Slowdown\n");
                    if (reasons & 0x40) printf("    - HW Thermal Slowdown\n");
                    if (reasons & 0x80) printf("    - HW Power Brake\n");
                    if (reasons & 0x100) printf("    - Display Clock Setting\n");
                }
            }

            /* Thermal settings */
            nvml_dev_buf_t getThermSettings = dlsym(lib, "nvmlDeviceGetThermalSettings");
            if (getThermSettings) {
                printf("\n  ThermalSettings:\n");
                uint8_t buf[256];
                memset(buf, 0, sizeof(buf));
                /* Signature: (device, sensorIndex, nvmlGpuThermalSettings_t*) */
                typedef nvmlReturn_t (*nvml_therm_t)(nvmlDevice_t, unsigned int, void *);
                nvml_therm_t getTS = (nvml_therm_t)getThermSettings;
                SAFE_CALL(getTS(dev, 0, buf), "getThermSettings");
                printf("    Status: %s\n", ret_str(ret, errStr));
                if (ret == NVML_SUCCESS) {
                    dump_u32_fields(buf, 128, "f");
                }
            }
        }
        printf("\n");

        /* -----------------------------------------------------------
         * 14. PowerMizerMode (verify on new driver)
         * ----------------------------------------------------------- */
        printf("--- 14. PowerMizerMode ---\n");
        {
            nvml_dev_buf_t getPMMode = dlsym(lib, "nvmlDeviceGetPowerMizerMode_v1");
            if (getPMMode) {
                uint8_t buf[32];
                memset(buf, 0, sizeof(buf));
                SAFE_CALL(getPMMode(dev, buf), "getPMMode");
                printf("  Status: %s\n", ret_str(ret, errStr));
                if (ret == NVML_SUCCESS) {
                    dump_u32_fields(buf, 12, "f");
                }
            }
        }
        printf("\n");

        /* -----------------------------------------------------------
         * 15. Architecture & GPU Info
         * ----------------------------------------------------------- */
        printf("--- 15. GPU Info ---\n");
        {
            nvml_dev_uint_t getArch = dlsym(lib, "nvmlDeviceGetArchitecture");
            nvml_dev_uint_t getCores = dlsym(lib, "nvmlDeviceGetNumGpuCores");
            nvml_dev_uint_t getBusWidth = dlsym(lib, "nvmlDeviceGetMemoryBusWidth");
            
            if (getArch) {
                unsigned int arch = 0;
                SAFE_CALL(getArch(dev, &arch), "getArch");
                printf("  Architecture: %u [%s]\n", arch, ret_str(ret, errStr));
            }
            if (getCores) {
                unsigned int cores = 0;
                SAFE_CALL(getCores(dev, &cores), "getCores");
                printf("  CUDA cores: %u [%s]\n", cores, ret_str(ret, errStr));
            }
            if (getBusWidth) {
                unsigned int bw = 0;
                SAFE_CALL(getBusWidth(dev, &bw), "getBusWidth");
                printf("  Memory bus width: %u bits [%s]\n", bw, ret_str(ret, errStr));
            }

            /* Board part number */
            typedef nvmlReturn_t (*nvml_str_t)(nvmlDevice_t, char *, unsigned int);
            nvml_str_t getBPN = dlsym(lib, "nvmlDeviceGetBoardPartNumber");
            if (getBPN) {
                char bpn[256] = {0};
                SAFE_CALL(getBPN(dev, bpn, 256), "getBPN");
                printf("  Board Part Number: %s [%s]\n", bpn, ret_str(ret, errStr));
            }
        }
        printf("\n");

        /* -----------------------------------------------------------
         * 16. Adaptive Clock Info
         * ----------------------------------------------------------- */
        printf("--- 16. Adaptive Clock ---\n");
        {
            nvml_dev_uint_t getAdaptive = dlsym(lib, "nvmlDeviceGetAdaptiveClockInfoStatus");
            if (getAdaptive) {
                unsigned int status = 0;
                SAFE_CALL(getAdaptive(dev, &status), "getAdaptive");
                printf("  AdaptiveClockInfoStatus: %u [%s]\n", status, ret_str(ret, errStr));
            }
        }
        printf("\n");

        /* -----------------------------------------------------------
         * 17. Check for P-state support 
         * ----------------------------------------------------------- */
        printf("--- 17. P-State Support ---\n");
        {
            /* nvmlDeviceGetSupportedClocksThrottleReasons */
            typedef nvmlReturn_t (*nvml_throttle_t)(nvmlDevice_t, unsigned long long *);
            nvml_throttle_t getSupportedThrottle = dlsym(lib, "nvmlDeviceGetSupportedClocksEventReasons");
            if (getSupportedThrottle) {
                unsigned long long reasons = 0;
                SAFE_CALL(getSupportedThrottle(dev, &reasons), "getSupportedThrottle");
                printf("  Supported throttle reasons: 0x%016llX [%s]\n", reasons, ret_str(ret, errStr));
            }

            /* Current P-state */
            typedef nvmlReturn_t (*nvml_pstate_t)(nvmlDevice_t, unsigned int *);
            nvml_pstate_t getPstate = dlsym(lib, "nvmlDeviceGetPerformanceState");
            if (getPstate) {
                unsigned int pstate = 0;
                SAFE_CALL(getPstate(dev, &pstate), "getPstate");
                printf("  Current P-state: P%u [%s]\n", pstate, ret_str(ret, errStr));
            }

            /* Supported P-states (via clock enum) */
            typedef nvmlReturn_t (*nvml_clocks_t)(nvmlDevice_t, unsigned int *, unsigned int *);
            nvml_clocks_t getSupportedClocks = dlsym(lib, "nvmlDeviceGetSupportedMemoryClocks");
            if (getSupportedClocks) {
                unsigned int count = 0;
                SAFE_CALL(getSupportedClocks(dev, &count, NULL), "getSupportedMemClocks");
                printf("  Supported memory clock count: %u [%s]\n", count, ret_str(ret, errStr));
                if (ret == NVML_SUCCESS && count > 0 && count < 64) {
                    unsigned int clocks[64];
                    SAFE_CALL(getSupportedClocks(dev, &count, clocks), "getSupportedMemClocks2");
                    if (ret == NVML_SUCCESS) {
                        printf("  Memory clocks:");
                        for (unsigned int i = 0; i < count && i < 16; i++)
                            printf(" %u", clocks[i]);
                        printf(" MHz\n");
                    }
                }
            }
        }
        printf("\n");

        /* -----------------------------------------------------------
         * 18. NEW: Check for Pstates20 support
         * ----------------------------------------------------------- */
        printf("--- 18. Pstates20 ---\n");
        {
            nvml_dev_buf_t getPstates20 = dlsym(lib, "nvmlDeviceGetPstates20");
            if (getPstates20) {
                /* Try known sizes */
                int found = 0;
                for (unsigned int ver = 1; ver <= 3 && !found; ver++) {
                    for (unsigned int sz = 8; sz <= 2048; sz += 4) {
                        uint8_t buf[4096];
                        memset(buf, 0, sizeof(buf));
                        uint32_t vword = NVML_VER(sz, ver);
                        memcpy(buf, &vword, 4);
                        SAFE_CALL(getPstates20(dev, buf), "getPstates20");
                        if (ret == NVML_SUCCESS) {
                            printf("  FOUND: sz=%u, ver=%u\n", sz, ver);
                            dump_u32_fields(buf, (sz > 256 ? 256 : sz), "f");
                            found = 1;
                            break;
                        } else if (ret == NVML_ERROR_NOT_SUPPORTED) {
                            found = -1;
                            break;
                        }
                    }
                    if (found) break;
                }
                if (found == -1) printf("  NOT SUPPORTED on this GPU\n");
                if (!found) printf("  No valid version/size found\n");
            } else {
                printf("  NOT EXPORTED\n");
            }
        }
        printf("\n");

        /* -----------------------------------------------------------
         * 19. Memory info & temperature
         * ----------------------------------------------------------- */
        printf("--- 19. Memory & Temperature ---\n");
        {
            typedef nvmlReturn_t (*nvml_temp_t)(nvmlDevice_t, unsigned int, unsigned int *);
            nvml_temp_t getTemp = dlsym(lib, "nvmlDeviceGetTemperature");
            if (getTemp) {
                unsigned int gpu_temp = 0, mem_temp = 0;
                SAFE_CALL(getTemp(dev, 0, &gpu_temp), "getTemp-GPU");
                printf("  GPU Temp: %u°C [%s]\n", gpu_temp, ret_str(ret, errStr));
                SAFE_CALL(getTemp(dev, 1, &mem_temp), "getTemp-Mem");  /* sensorType 1 = NVML_TEMPERATURE_COUNT only known are 0=GPU */
                printf("  Mem Temp: %u°C [%s]\n", mem_temp, ret_str(ret, errStr));
                /* Try more sensor types */
                for (unsigned int s = 2; s <= 10; s++) {
                    unsigned int t = 0;
                    SAFE_CALL(getTemp(dev, s, &t), "getTemp");
                    if (ret == NVML_SUCCESS)
                        printf("  Sensor %u: %u°C\n", s, t);
                }
            }

            /* Temperature thresholds */
            typedef nvmlReturn_t (*nvml_temp_thresh_t)(nvmlDevice_t, unsigned int, unsigned int *);
            nvml_temp_thresh_t getThresh = dlsym(lib, "nvmlDeviceGetTemperatureThreshold");
            if (getThresh) {
                const char *tnames[] = {"Shutdown", "Slowdown", "MemMax", "GpuMax", "AcousticMin",
                                        "AcousticCurr", "AcousticMax"};
                for (unsigned int t = 0; t <= 6; t++) {
                    unsigned int thresh = 0;
                    SAFE_CALL(getThresh(dev, t, &thresh), "getThresh");
                    if (ret == NVML_SUCCESS)
                        printf("  Threshold %s: %u°C\n", tnames[t], thresh);
                    else if (ret != NVML_ERROR_NOT_SUPPORTED)
                        printf("  Threshold %s: %s\n", tnames[t], ret_str(ret, errStr));
                }
            }
        }
        printf("\n");

    } /* end for each GPU */

    printf("\n========================================\n");
    printf("=== NvAPI Function ID Probe ===\n");
    printf("========================================\n\n");

    /* Probe NvAPI library for known OC-related function IDs */
    void *nvapi = dlopen("libnvidia-api.so.1", RTLD_LAZY);
    if (nvapi) {
        typedef void *(*nvapi_qi_t)(unsigned int);
        nvapi_qi_t qi = dlsym(nvapi, "nvapi_QueryInterface");
        if (qi) {
            /* Known function IDs from previous research + new candidates */
            struct {
                unsigned int id;
                const char *name;
            } funcs[] = {
                /* Previously working */
                {0xDCB616C3, "GPU_GetAllClockFrequencies"},
                {0x465F9BCF, "GPU_GetVoltage"},
                {0x60DED2ED, "GPU_ClientPowerTopologyGetInfo"},
                {0x3D358A0C, "GPU_PerfPoliciesGetStatus"},
                {0x65FE3AAD, "GPU_GetThermalInfo"},
                {0x0D258BB5, "GPU_Unknown_ReferenceClocks"},

                /* ClockBoost family - were unavailable on Blackwell */
                {0x23F1B133, "GPU_GetClockBoostTable"},
                {0x507B4B59, "GPU_GetClockBoostTable_alt"},
                {0x21537AD4, "GPU_GetVFPCurve"},
                {0x7F5F90A7, "GPU_GetVFPCurve_alt"},
                {0x0C0B2B96, "GPU_GetClockBoostLock"},
                {0x39944e71, "GPU_GetClockBoostMask"},
                {0xE5AC921F, "GPU_SetClockBoostLock"},
                {0x74819072, "GPU_SetClockBoostTable"},
                {0x0733D009, "GPU_GetClockBoostRanges"},
                {0x34206D86, "GPU_GetClockTable"},

                /* Power/Voltage related IDs */
                {0xDA4A8A28, "GPU_ClientPowerPoliciesGetInfo"},
                {0x34206D86, "GPU_ClientPowerPoliciesGetStatus"},
                {0xAD95F5ED, "GPU_ClientPowerPoliciesSetStatus"},
                {0x28E5B430, "GPU_GetThermalPoliciesInfo"},
                {0xE9C425A1, "GPU_GetThermalPoliciesStatus"},
                {0x034C0B13, "GPU_SetThermalPoliciesStatus"},

                /* Pstates20 */
                {0x6FF81213, "GPU_GetPstates20"},
                {0x0F4DAC4F, "GPU_SetPstates20"},
                {0x1F7B35A2, "GPU_GetPerformanceDecreaseInfo"},
                {0x60DED2ED, "GPU_ClientPowerTopology"},

                /* Fan control */
                {0xA084F1A0, "GPU_GetCoolerSettings"},
                {0x891FA0AE, "GPU_SetCoolerLevels"},
                {0xDA141340, "GPU_GetCoolerPolicyTable"},
                {0x987947CD, "GPU_SetCoolerPolicyTable"},
                {0x35AED5E8, "GPU_RestoreCoolerSettings"},
                {0x18AD8264, "GPU_GetCoolerInfo"},

                /* Overclocking */
                {0xE3640A56, "GPU_GetPerfClocks"},
                {0x7D239A04, "GPU_GetOverclockRange"},
                {0xB3A6E3B3, "GPU_GetPstateInfo"},

                /* New candidates - common NvAPI IDs */
                {0x2DDFB66E, "GPU_GetPCIInfo"},
                {0xF951A4D1, "GPU_GetMaxBoostClock"},
                {0x189B149C, "GPU_GetTotalMemory"},
                {0xCEEFBFAE, "GPU_GetClockBoostTableV2"},
                {0x3873B452, "GPU_SetVFPCurve"},

                /* ThermChannel functions (seen in strings) */
                {0x0, "GPU_ThermChannelGetControl"},  /* need to find real ID */
                {0x0, "GPU_ThermChannelGetInfo"},
                {0x0, "GPU_ThermChannelGetStatus"},

                /* Sentinel */
                {0, NULL},
            };

            for (int i = 0; funcs[i].name; i++) {
                if (funcs[i].id == 0) continue;
                void *fn = qi(funcs[i].id);
                printf("  0x%08X %-45s %s\n", funcs[i].id, funcs[i].name,
                       fn ? "FOUND" : "not available");
            }

            /* Brute-force scan for ThermChannel functions by trying common ID patterns */
            printf("\n  --- Scanning for ThermChannel* IDs ---\n");
            /* Try to find by checking strings like "NvAPI_GPU_ThermChannel" which log version info */
            /* The fact that the strings exist means the function handler IS in the NvAPI dispatch table */
            /* Let's try a focused range scan */
            int therm_found = 0;
            /* Scan specific ranges that are common for GPU thermal functions */
            /* Actually let's try a smarter approach - we know the NvAPI dispatch table has ~200 entries typically */
            /* Let's just probe a large range and see what sticks */

            printf("\n  --- Probing NvAPI for new function IDs (broad scan) ---\n");
            /* We'll focus on ranges around known thermal/power function IDs */

            /* Quick scan around known IDs */
            unsigned int known_ids[] = {
                0x65FE3AAD, /* GPU_GetThermalInfo (known working) */
                0x28E5B430, /* GPU_GetThermalPoliciesInfo */
                0xE9C425A1, /* GPU_GetThermalPoliciesStatus */
                0x034C0B13, /* GPU_SetThermalPoliciesStatus */
                0x0D258BB5, /* Unknown ref clocks */
            };
            for (int k = 0; k < 5; k++) {
                unsigned int base = known_ids[k] & 0xFFFF0000;
                for (unsigned int off = 0; off < 0x10000; off += 0x100) {
                    unsigned int test_id = base | off;
                    if (test_id == known_ids[k]) continue;
                    void *fn = qi(test_id);
                    if (fn) {
                        /* Check if this is one of our already-known IDs */
                        int is_known = 0;
                        for (int j = 0; funcs[j].name; j++) {
                            if (funcs[j].id == test_id) { is_known = 1; break; }
                        }
                        if (!is_known) {
                            printf("  NEW FUNCTION: 0x%08X (near known 0x%08X)\n", test_id, known_ids[k]);
                            therm_found++;
                        }
                    }
                }
            }
            if (!therm_found) printf("  No new functions found in scanned ranges\n");

        } else {
            printf("nvapi_QueryInterface not found\n");
        }
        dlclose(nvapi);
    } else {
        printf("libnvidia-api.so.1 not available\n");
    }

    nvmlShutdown();
    dlclose(lib);

    printf("\n=== Probe complete ===\n");
    return 0;
}
