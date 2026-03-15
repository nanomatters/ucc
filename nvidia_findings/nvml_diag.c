/*
 * Quick NVML diagnostic - check what data is readable on RTX 5090
 * Compile: gcc -o nvml_diag nvml_diag.c -ldl
 * Run:     sudo ./nvml_diag
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

typedef void* nvmlDevice_t;
typedef unsigned int nvmlReturn_t;

#define NVML_SUCCESS 0
#define NVML_CLOCK_GRAPHICS 0
#define NVML_CLOCK_SM 1
#define NVML_CLOCK_MEM 2
#define NVML_PSTATE_UNKNOWN 32

typedef nvmlReturn_t (*nvmlInit_t)(void);
typedef nvmlReturn_t (*nvmlShutdown_t)(void);
typedef nvmlReturn_t (*nvmlDeviceGetCount_t)(unsigned int*);
typedef nvmlReturn_t (*nvmlDeviceGetHandleByIndex_t)(unsigned int, nvmlDevice_t*);
typedef nvmlReturn_t (*nvmlDeviceGetName_t)(nvmlDevice_t, char*, unsigned int);
typedef nvmlReturn_t (*nvmlDeviceGetTemperature_t)(nvmlDevice_t, unsigned int, unsigned int*);
typedef nvmlReturn_t (*nvmlDeviceGetPowerUsage_t)(nvmlDevice_t, unsigned int*);
typedef nvmlReturn_t (*nvmlDeviceGetClockInfo_t)(nvmlDevice_t, unsigned int, unsigned int*);
typedef nvmlReturn_t (*nvmlDeviceGetMaxClockInfo_t)(nvmlDevice_t, unsigned int, unsigned int*);
typedef nvmlReturn_t (*nvmlDeviceGetPerformanceState_t)(nvmlDevice_t, unsigned int*);
typedef nvmlReturn_t (*nvmlDeviceGetPowerManagementLimit_t)(nvmlDevice_t, unsigned int*);
typedef nvmlReturn_t (*nvmlDeviceGetPowerManagementLimitConstraints_t)(nvmlDevice_t, unsigned int*, unsigned int*);
typedef nvmlReturn_t (*nvmlDeviceGetPowerManagementDefaultLimit_t)(nvmlDevice_t, unsigned int*);
typedef nvmlReturn_t (*nvmlDeviceGetEnforcedPowerLimit_t)(nvmlDevice_t, unsigned int*);
typedef nvmlReturn_t (*nvmlDeviceGetFanSpeed_t)(nvmlDevice_t, unsigned int*);
typedef nvmlReturn_t (*nvmlDeviceGetFanSpeed_v2_t)(nvmlDevice_t, unsigned int, unsigned int*);
typedef nvmlReturn_t (*nvmlDeviceGetNumFans_t)(nvmlDevice_t, unsigned int*);
typedef nvmlReturn_t (*nvmlDeviceGetSupportedPerformanceStates_t)(nvmlDevice_t, unsigned int*, unsigned int);
typedef nvmlReturn_t (*nvmlDeviceGetMinMaxClockOfPState_t)(nvmlDevice_t, unsigned int, unsigned int, unsigned int*, unsigned int*);

typedef struct {
    unsigned int version;
    unsigned int type;
    unsigned int pstate;
    int clockOffsetMHz;
    int minClockOffsetMHz;
    int maxClockOffsetMHz;
} nvmlClockOffset_t;
typedef nvmlReturn_t (*nvmlDeviceGetClockOffsets_t)(nvmlDevice_t, nvmlClockOffset_t*);

typedef struct {
    unsigned int gpu;
    unsigned int memory;
} nvmlUtilization_t;
typedef nvmlReturn_t (*nvmlDeviceGetUtilizationRates_t)(nvmlDevice_t, nvmlUtilization_t*);

int main(void) {
    void *lib = dlopen("libnvidia-ml.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "Cannot load libnvidia-ml.so.1: %s\n", dlerror()); return 1; }

    nvmlInit_t init = (nvmlInit_t)dlsym(lib, "nvmlInit_v2");
    nvmlShutdown_t shutdown = (nvmlShutdown_t)dlsym(lib, "nvmlShutdown");
    nvmlDeviceGetCount_t getCount = (nvmlDeviceGetCount_t)dlsym(lib, "nvmlDeviceGetCount_v2");
    nvmlDeviceGetHandleByIndex_t getHandle = (nvmlDeviceGetHandleByIndex_t)dlsym(lib, "nvmlDeviceGetHandleByIndex_v2");
    nvmlDeviceGetName_t getName = (nvmlDeviceGetName_t)dlsym(lib, "nvmlDeviceGetName");
    nvmlDeviceGetTemperature_t getTemp = (nvmlDeviceGetTemperature_t)dlsym(lib, "nvmlDeviceGetTemperature");
    nvmlDeviceGetPowerUsage_t getPower = (nvmlDeviceGetPowerUsage_t)dlsym(lib, "nvmlDeviceGetPowerUsage");
    nvmlDeviceGetClockInfo_t getClockInfo = (nvmlDeviceGetClockInfo_t)dlsym(lib, "nvmlDeviceGetClockInfo");
    nvmlDeviceGetMaxClockInfo_t getMaxClockInfo = (nvmlDeviceGetMaxClockInfo_t)dlsym(lib, "nvmlDeviceGetMaxClockInfo");
    nvmlDeviceGetPerformanceState_t getPstate = (nvmlDeviceGetPerformanceState_t)dlsym(lib, "nvmlDeviceGetPerformanceState");
    nvmlDeviceGetPowerManagementLimit_t getPowerLimit = (nvmlDeviceGetPowerManagementLimit_t)dlsym(lib, "nvmlDeviceGetPowerManagementLimit");
    nvmlDeviceGetPowerManagementLimitConstraints_t getPowerConstraints = (nvmlDeviceGetPowerManagementLimitConstraints_t)dlsym(lib, "nvmlDeviceGetPowerManagementLimitConstraints");
    nvmlDeviceGetPowerManagementDefaultLimit_t getPowerDefault = (nvmlDeviceGetPowerManagementDefaultLimit_t)dlsym(lib, "nvmlDeviceGetPowerManagementDefaultLimit");
    nvmlDeviceGetEnforcedPowerLimit_t getEnforced = (nvmlDeviceGetEnforcedPowerLimit_t)dlsym(lib, "nvmlDeviceGetEnforcedPowerLimit");
    nvmlDeviceGetFanSpeed_t getFanSpeed = (nvmlDeviceGetFanSpeed_t)dlsym(lib, "nvmlDeviceGetFanSpeed");
    nvmlDeviceGetFanSpeed_v2_t getFanSpeedV2 = (nvmlDeviceGetFanSpeed_v2_t)dlsym(lib, "nvmlDeviceGetFanSpeed_v2");
    nvmlDeviceGetNumFans_t getNumFans = (nvmlDeviceGetNumFans_t)dlsym(lib, "nvmlDeviceGetNumFans");
    nvmlDeviceGetSupportedPerformanceStates_t getSupportedPstates = (nvmlDeviceGetSupportedPerformanceStates_t)dlsym(lib, "nvmlDeviceGetSupportedPerformanceStates");
    nvmlDeviceGetMinMaxClockOfPState_t getMinMaxClock = (nvmlDeviceGetMinMaxClockOfPState_t)dlsym(lib, "nvmlDeviceGetMinMaxClockOfPState");
    nvmlDeviceGetClockOffsets_t getClockOffsets = (nvmlDeviceGetClockOffsets_t)dlsym(lib, "nvmlDeviceGetClockOffsets");
    nvmlDeviceGetUtilizationRates_t getUtil = (nvmlDeviceGetUtilizationRates_t)dlsym(lib, "nvmlDeviceGetUtilizationRates");

    if (!init || !shutdown || !getCount || !getHandle) {
        fprintf(stderr, "Missing critical NVML symbols\n");
        return 1;
    }

    nvmlReturn_t ret = init();
    if (ret != NVML_SUCCESS) { fprintf(stderr, "nvmlInit failed: %u\n", ret); return 1; }

    unsigned int count = 0;
    getCount(&count);
    printf("GPU count: %u\n\n", count);

    for (unsigned int i = 0; i < count; i++) {
        nvmlDevice_t dev = NULL;
        if (getHandle(i, &dev) != NVML_SUCCESS) { printf("GPU %u: getHandle FAILED\n", i); continue; }

        char name[256] = {};
        if (getName) { ret = getName(dev, name, sizeof(name)); printf("GPU %u: name=%s (ret=%u)\n", i, name, ret); }

        unsigned int val = 0;
        printf("\n--- Monitoring ---\n");

        if (getTemp) { val = 0; ret = getTemp(dev, 0, &val); printf("  Temperature:       ret=%u  val=%u °C\n", ret, val); }
        if (getPower) { val = 0; ret = getPower(dev, &val); printf("  PowerUsage:        ret=%u  val=%u mW (%.1f W)\n", ret, val, val/1000.0); }
        if (getEnforced) { val = 0; ret = getEnforced(dev, &val); printf("  EnforcedPwrLimit:  ret=%u  val=%u mW (%.1f W)\n", ret, val, val/1000.0); }
        if (getPowerLimit) { val = 0; ret = getPowerLimit(dev, &val); printf("  PowerMgmtLimit:    ret=%u  val=%u mW (%.1f W)\n", ret, val, val/1000.0); }
        if (getPowerDefault) { val = 0; ret = getPowerDefault(dev, &val); printf("  PowerDefaultLimit: ret=%u  val=%u mW (%.1f W)\n", ret, val, val/1000.0); }
        if (getPowerConstraints) { unsigned int rmin=0,rmax=0; ret=getPowerConstraints(dev,&rmin,&rmax); printf("  PowerConstraints:  ret=%u  min=%u max=%u mW (%.1f-%.1f W)\n", ret, rmin, rmax, rmin/1000.0, rmax/1000.0); }

        if (getClockInfo) { val = 0; ret = getClockInfo(dev, NVML_CLOCK_GRAPHICS, &val); printf("  ClockInfo(GR):     ret=%u  val=%u MHz\n", ret, val); }
        if (getClockInfo) { val = 0; ret = getClockInfo(dev, NVML_CLOCK_SM, &val); printf("  ClockInfo(SM):     ret=%u  val=%u MHz\n", ret, val); }
        if (getClockInfo) { val = 0; ret = getClockInfo(dev, NVML_CLOCK_MEM, &val); printf("  ClockInfo(MEM):    ret=%u  val=%u MHz\n", ret, val); }
        if (getMaxClockInfo) { val = 0; ret = getMaxClockInfo(dev, NVML_CLOCK_GRAPHICS, &val); printf("  MaxClockInfo(GR):  ret=%u  val=%u MHz\n", ret, val); }
        if (getMaxClockInfo) { val = 0; ret = getMaxClockInfo(dev, NVML_CLOCK_MEM, &val); printf("  MaxClockInfo(MEM): ret=%u  val=%u MHz\n", ret, val); }

        if (getPstate) { val = 99; ret = getPstate(dev, &val); printf("  PerfState:         ret=%u  val=%u\n", ret, val); }

        if (getUtil) { nvmlUtilization_t u = {}; ret = getUtil(dev, &u); printf("  Utilization:       ret=%u  gpu=%u%% mem=%u%%\n", ret, u.gpu, u.memory); }

        if (getNumFans) { val = 0; ret = getNumFans(dev, &val); printf("  NumFans:           ret=%u  val=%u\n", ret, val); }
        if (getFanSpeed) { val = 0; ret = getFanSpeed(dev, &val); printf("  FanSpeed(legacy):  ret=%u  val=%u%%\n", ret, val); }
        if (getFanSpeedV2) { 
            unsigned int numFans = 0;
            if (getNumFans) getNumFans(dev, &numFans);
            for (unsigned int f = 0; f < (numFans > 0 ? numFans : 1); f++) {
                val = 0; ret = getFanSpeedV2(dev, f, &val); 
                printf("  FanSpeed_v2[%u]:    ret=%u  val=%u%%\n", f, ret, val); 
            }
        }

        printf("\n--- P-States ---\n");
        if (getSupportedPstates) {
            unsigned int pstates[16];
            memset(pstates, 0xFF, sizeof(pstates));
            ret = getSupportedPstates(dev, pstates, 16);
            printf("  SupportedPstates:  ret=%u  states=", ret);
            for (int p = 0; p < 16 && pstates[p] != 32; p++) printf("P%u ", pstates[p]);
            printf("\n");
        }

        if (getMinMaxClock) {
            printf("\n--- MinMaxClockOfPState ---\n");
            unsigned int pstates[16];
            memset(pstates, 0xFF, sizeof(pstates));
            if (getSupportedPstates) getSupportedPstates(dev, pstates, 16);
            for (int p = 0; p < 16 && pstates[p] != 32; p++) {
                unsigned int gmin=0, gmax=0, mmin=0, mmax=0;
                ret = getMinMaxClock(dev, NVML_CLOCK_GRAPHICS, pstates[p], &gmin, &gmax);
                nvmlReturn_t ret2 = getMinMaxClock(dev, NVML_CLOCK_MEM, pstates[p], &mmin, &mmax);
                printf("  P%u: GPU ret=%u min=%u max=%u MHz  |  MEM ret=%u min=%u max=%u MHz\n", 
                       pstates[p], ret, gmin, gmax, ret2, mmin, mmax);
            }
        }

        if (getClockOffsets) {
            printf("\n--- ClockOffsets ---\n");
            for (int ct = 0; ct <= 2; ct += 2) { // GRAPHICS=0, MEM=2
                nvmlClockOffset_t off = {};
                off.version = 0x01000018; // ver1 = (1<<24) | sizeof(24)
                off.type = ct;
                off.pstate = 0; // P0
                ret = getClockOffsets(dev, &off);
                printf("  type=%d P0: ret=%u offset=%d min=%d max=%d\n", ct, ret, off.clockOffsetMHz, off.minClockOffsetMHz, off.maxClockOffsetMHz);
            }
        }
    }

    shutdown();
    dlclose(lib);
    return 0;
}
