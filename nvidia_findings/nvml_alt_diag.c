/*
 * NVML Alternative API Diagnostic for Blackwell RTX 5090
 * Tests alternative monitoring functions when standard ones return NOT_SUPPORTED
 * Compile: gcc -o nvml_alt_diag nvml_alt_diag.c -ldl
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>

typedef void* nvmlDevice_t;
typedef unsigned int nvmlReturn_t;
#define NVML_SUCCESS 0

typedef nvmlReturn_t (*nvmlInit_t)(void);
typedef nvmlReturn_t (*nvmlShutdown_t)(void);
typedef nvmlReturn_t (*nvmlDeviceGetCount_t)(unsigned int*);
typedef nvmlReturn_t (*nvmlDeviceGetHandleByIndex_t)(unsigned int, nvmlDevice_t*);
typedef nvmlReturn_t (*nvmlDeviceGetName_t)(nvmlDevice_t, char*, unsigned int);

/* Standard APIs (for comparison) */
typedef nvmlReturn_t (*nvmlDeviceGetClockInfo_t)(nvmlDevice_t, unsigned int, unsigned int*);
typedef nvmlReturn_t (*nvmlDeviceGetPowerUsage_t)(nvmlDevice_t, unsigned int*);

/* Alternative clock APIs */
typedef nvmlReturn_t (*nvmlDeviceGetCurrentClockFreqs_t)(nvmlDevice_t, unsigned int*, unsigned int*);
/* nvmlDeviceGetClock(device, clockType, clockId, clockMHz) */
typedef nvmlReturn_t (*nvmlDeviceGetClock_t)(nvmlDevice_t, unsigned int, unsigned int, unsigned int*);

/* Energy consumption */
typedef nvmlReturn_t (*nvmlDeviceGetTotalEnergyConsumption_t)(nvmlDevice_t, unsigned long long*);

/* Alternative fan APIs */
typedef nvmlReturn_t (*nvmlDeviceGetCoolerInfo_t)(nvmlDevice_t, void*);

/* Dynamic pstates */
typedef struct {
    unsigned int isEnabled;
    unsigned int percentage;
    unsigned int incThreshold;
    unsigned int decThreshold;
} nvmlDynamicPstateUtilization_t;

typedef struct {
    unsigned int flags;
    nvmlDynamicPstateUtilization_t utilization[8];
} nvmlDynamicPstatesInfo_t;
typedef nvmlReturn_t (*nvmlDeviceGetDynamicPstatesInfo_t)(nvmlDevice_t, nvmlDynamicPstatesInfo_t*);

/* Power source */
typedef nvmlReturn_t (*nvmlDeviceGetPowerSource_t)(nvmlDevice_t, unsigned int*);

/* Field values */
typedef struct {
    unsigned int fieldId;
    unsigned int scopeId;
    long long timestamp;
    long long latencyUsec;
    union {
        double dVal;
        unsigned int uiVal;
        unsigned long long ulVal;
        long long sllVal;
        char strVal[16];
    } value;
    nvmlReturn_t nvmlReturn;
    unsigned int valueType;
} nvmlFieldValue_t;
typedef nvmlReturn_t (*nvmlDeviceGetFieldValues_t)(nvmlDevice_t, int, nvmlFieldValue_t*);

/* Adaptive clock */
typedef nvmlReturn_t (*nvmlDeviceGetAdaptiveClockInfoStatus_t)(nvmlDevice_t, unsigned int, unsigned int*);

/* Applications clock */
typedef nvmlReturn_t (*nvmlDeviceGetApplicationsClock_t)(nvmlDevice_t, unsigned int, unsigned int*);
typedef nvmlReturn_t (*nvmlDeviceGetDefaultApplicationsClock_t)(nvmlDevice_t, unsigned int, unsigned int*);
typedef nvmlReturn_t (*nvmlDeviceGetMaxCustomerBoostClock_t)(nvmlDevice_t, unsigned int, unsigned int*);

/* Supported clocks */
typedef nvmlReturn_t (*nvmlDeviceGetSupportedGraphicsClocks_t)(nvmlDevice_t, unsigned int, unsigned int*, unsigned int*);
typedef nvmlReturn_t (*nvmlDeviceGetSupportedMemoryClocks_t)(nvmlDevice_t, unsigned int*, unsigned int*);

int main(void) {
    void *lib = dlopen("libnvidia-ml.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "Cannot load libnvidia-ml.so.1\n"); return 1; }

    nvmlInit_t init = (nvmlInit_t)dlsym(lib, "nvmlInit_v2");
    nvmlShutdown_t shutdown = (nvmlShutdown_t)dlsym(lib, "nvmlShutdown");
    nvmlDeviceGetCount_t getCount = (nvmlDeviceGetCount_t)dlsym(lib, "nvmlDeviceGetCount_v2");
    nvmlDeviceGetHandleByIndex_t getHandle = (nvmlDeviceGetHandleByIndex_t)dlsym(lib, "nvmlDeviceGetHandleByIndex_v2");

    if (!init || !shutdown || !getCount || !getHandle) { fprintf(stderr, "Missing NVML core\n"); return 1; }
    if (init() != NVML_SUCCESS) { fprintf(stderr, "nvmlInit failed\n"); return 1; }

    unsigned int count = 0;
    getCount(&count);
    
    nvmlDevice_t dev = NULL;
    if (getHandle(0, &dev) != NVML_SUCCESS) { fprintf(stderr, "getHandle failed\n"); return 1; }

    nvmlReturn_t ret;
    printf("=== Alternative NVML APIs for RTX 5090 Blackwell ===\n\n");

    /* Standard APIs (baseline) */
    nvmlDeviceGetClockInfo_t getClockInfo = (nvmlDeviceGetClockInfo_t)dlsym(lib, "nvmlDeviceGetClockInfo");
    nvmlDeviceGetPowerUsage_t getPowerUsage = (nvmlDeviceGetPowerUsage_t)dlsym(lib, "nvmlDeviceGetPowerUsage");
    
    if (getClockInfo) {
        unsigned int v=0; ret=getClockInfo(dev,0,&v);
        printf("[baseline] nvmlDeviceGetClockInfo(GR):  ret=%u val=%u\n", ret, v);
    }
    if (getPowerUsage) {
        unsigned int v=0; ret=getPowerUsage(dev,&v);
        printf("[baseline] nvmlDeviceGetPowerUsage:     ret=%u val=%u mW\n", ret, v);
    }

    /* nvmlDeviceGetCurrentClockFreqs */
    nvmlDeviceGetCurrentClockFreqs_t getCurrentClockFreqs = 
        (nvmlDeviceGetCurrentClockFreqs_t)dlsym(lib, "nvmlDeviceGetCurrentClockFreqs");
    if (getCurrentClockFreqs) {
        unsigned int gfxClk=0, memClk=0;
        ret = getCurrentClockFreqs(dev, &gfxClk, &memClk);
        printf("[alt] nvmlDeviceGetCurrentClockFreqs:   ret=%u gfx=%u MHz mem=%u MHz\n", ret, gfxClk, memClk);
    } else printf("[alt] nvmlDeviceGetCurrentClockFreqs:   NOT EXPORTED\n");

    /* nvmlDeviceGetClock - try different clockId values */
    nvmlDeviceGetClock_t getClock = (nvmlDeviceGetClock_t)dlsym(lib, "nvmlDeviceGetClock");
    if (getClock) {
        printf("[alt] nvmlDeviceGetClock:\n");
        for (unsigned int clockType = 0; clockType <= 3; clockType++) {
            for (unsigned int clockId = 0; clockId <= 2; clockId++) {
                unsigned int clk = 0;
                ret = getClock(dev, clockType, clockId, &clk);
                if (ret == NVML_SUCCESS)
                    printf("      type=%u id=%u: ret=%u val=%u MHz\n", clockType, clockId, ret, clk);
            }
        }
        /* Try all types to see which return success */
        int anySuccess = 0;
        for (unsigned int clockType = 0; clockType <= 3; clockType++) {
            for (unsigned int clockId = 0; clockId <= 2; clockId++) {
                unsigned int clk = 0;
                ret = getClock(dev, clockType, clockId, &clk);
                if (ret != NVML_SUCCESS && !anySuccess)
                    printf("      type=%u id=%u: ret=%u (FAILED)\n", clockType, clockId, ret);
            }
        }
    } else printf("[alt] nvmlDeviceGetClock:               NOT EXPORTED\n");
    
    /* nvmlDeviceGetTotalEnergyConsumption */
    nvmlDeviceGetTotalEnergyConsumption_t getEnergy = 
        (nvmlDeviceGetTotalEnergyConsumption_t)dlsym(lib, "nvmlDeviceGetTotalEnergyConsumption");
    if (getEnergy) {
        unsigned long long e1=0, e2=0;
        ret = getEnergy(dev, &e1);
        printf("[alt] nvmlDeviceGetTotalEnergyConsumption: ret=%u val=%llu mJ\n", ret, e1);
        if (ret == NVML_SUCCESS) {
            usleep(500000); /* 500ms */
            ret = getEnergy(dev, &e2);
            double powerW = (double)(e2 - e1) / 500.0; /* mJ / ms = W */
            printf("      after 500ms: %llu mJ  delta=%llu mJ  => ~%.1f W\n", e2, e2-e1, powerW);
        }
    } else printf("[alt] nvmlDeviceGetTotalEnergyConsumption: NOT EXPORTED\n");

    /* nvmlDeviceGetCoolerInfo (struct is undocumented) 
       Let's probe it with a large buffer and see raw bytes */
    nvmlDeviceGetCoolerInfo_t getCoolerInfo = 
        (nvmlDeviceGetCoolerInfo_t)dlsym(lib, "nvmlDeviceGetCoolerInfo");
    if (getCoolerInfo) {
        unsigned char buf[4096];
        memset(buf, 0xAA, sizeof(buf));
        
        /* Try with a version struct at the beginning */
        /* Many NVML structs start with version: (sizeof & 0xFFFF) | (ver << 16) */
        for (unsigned int ver = 1; ver <= 3; ver++) {
            memset(buf, 0xAA, sizeof(buf));
            for (unsigned int sz = 64; sz <= 2048; sz *= 2) {
                unsigned int version = (sz & 0xFFFF) | (ver << 16);
                memcpy(buf, &version, 4);
                ret = getCoolerInfo(dev, buf);
                if (ret == NVML_SUCCESS) {
                    printf("[alt] nvmlDeviceGetCoolerInfo: ret=%u ver=%u sz=%u\n", ret, ver, sz);
                    printf("      raw[0..63]: ");
                    for (int i = 0; i < 64; i++) printf("%02x ", buf[i]);
                    printf("\n");
                    goto cooler_done;
                }
                if (ret != 2) { /* not INVALID_ARG, stop trying sizes */
                    printf("[alt] nvmlDeviceGetCoolerInfo: ret=%u ver=%u sz=%u\n", ret, ver, sz);
                    break;
                }
            }
        }
        printf("[alt] nvmlDeviceGetCoolerInfo: all versions/sizes failed\n");
        cooler_done:;
    } else printf("[alt] nvmlDeviceGetCoolerInfo:          NOT EXPORTED\n");

    /* nvmlDeviceGetDynamicPstatesInfo */
    nvmlDeviceGetDynamicPstatesInfo_t getDynPstates = 
        (nvmlDeviceGetDynamicPstatesInfo_t)dlsym(lib, "nvmlDeviceGetDynamicPstatesInfo");
    if (getDynPstates) {
        nvmlDynamicPstatesInfo_t info = {};
        ret = getDynPstates(dev, &info);
        printf("[alt] nvmlDeviceGetDynamicPstatesInfo:  ret=%u flags=0x%x\n", ret, info.flags);
        if (ret == NVML_SUCCESS) {
            for (int i = 0; i < 8; i++) {
                if (info.utilization[i].isEnabled)
                    printf("      [%d] enabled=%u pct=%u%% incThresh=%u decThresh=%u\n", 
                           i, info.utilization[i].isEnabled, info.utilization[i].percentage,
                           info.utilization[i].incThreshold, info.utilization[i].decThreshold);
            }
        }
    } else printf("[alt] nvmlDeviceGetDynamicPstatesInfo:  NOT EXPORTED\n");

    /* nvmlDeviceGetPowerSource */
    nvmlDeviceGetPowerSource_t getPowerSource = 
        (nvmlDeviceGetPowerSource_t)dlsym(lib, "nvmlDeviceGetPowerSource");
    if (getPowerSource) {
        unsigned int src = 99;
        ret = getPowerSource(dev, &src);
        printf("[alt] nvmlDeviceGetPowerSource:         ret=%u val=%u\n", ret, src);
    } else printf("[alt] nvmlDeviceGetPowerSource:         NOT EXPORTED\n");

    /* Applications clock */
    nvmlDeviceGetApplicationsClock_t getAppClock = 
        (nvmlDeviceGetApplicationsClock_t)dlsym(lib, "nvmlDeviceGetApplicationsClock");
    if (getAppClock) {
        unsigned int v=0;
        ret = getAppClock(dev, 0, &v);
        printf("[alt] nvmlDeviceGetApplicationsClock(GR): ret=%u val=%u MHz\n", ret, v);
        v=0; ret = getAppClock(dev, 2, &v);
        printf("[alt] nvmlDeviceGetApplicationsClock(MEM): ret=%u val=%u MHz\n", ret, v);
    }

    /* Default applications clock */
    nvmlDeviceGetDefaultApplicationsClock_t getDefAppClock = 
        (nvmlDeviceGetDefaultApplicationsClock_t)dlsym(lib, "nvmlDeviceGetDefaultApplicationsClock");
    if (getDefAppClock) {
        unsigned int v=0;
        ret = getDefAppClock(dev, 0, &v);
        printf("[alt] nvmlDeviceGetDefaultAppClock(GR):  ret=%u val=%u MHz\n", ret, v);
        v=0; ret = getDefAppClock(dev, 2, &v);
        printf("[alt] nvmlDeviceGetDefaultAppClock(MEM): ret=%u val=%u MHz\n", ret, v);
    }

    /* Max customer boost clock */
    nvmlDeviceGetMaxCustomerBoostClock_t getMaxBoost = 
        (nvmlDeviceGetMaxCustomerBoostClock_t)dlsym(lib, "nvmlDeviceGetMaxCustomerBoostClock");
    if (getMaxBoost) {
        unsigned int v=0;
        ret = getMaxBoost(dev, 0, &v);
        printf("[alt] nvmlDeviceGetMaxCustomerBoostClock(GR): ret=%u val=%u MHz\n", ret, v);
        v=0; ret = getMaxBoost(dev, 2, &v);
        printf("[alt] nvmlDeviceGetMaxCustomerBoostClock(MEM): ret=%u val=%u MHz\n", ret, v);
    }

    /* Adaptive clock info status */
    nvmlDeviceGetAdaptiveClockInfoStatus_t getAdaptive = 
        (nvmlDeviceGetAdaptiveClockInfoStatus_t)dlsym(lib, "nvmlDeviceGetAdaptiveClockInfoStatus");
    if (getAdaptive) {
        unsigned int status = 99;
        ret = getAdaptive(dev, 0, &status);
        printf("[alt] nvmlDeviceGetAdaptiveClockInfoStatus: ret=%u val=%u\n", ret, status);
    }

    /* Supported memory clocks - to get at valid memory frequencies */
    nvmlDeviceGetSupportedMemoryClocks_t getSupportedMemClocks = 
        (nvmlDeviceGetSupportedMemoryClocks_t)dlsym(lib, "nvmlDeviceGetSupportedMemoryClocks");
    if (getSupportedMemClocks) {
        unsigned int cnt = 0;
        ret = getSupportedMemClocks(dev, &cnt, NULL);
        printf("[alt] nvmlDeviceGetSupportedMemoryClocks: ret=%u count=%u\n", ret, cnt);
        if (cnt > 0 && cnt < 100) {
            unsigned int *clocks = calloc(cnt, sizeof(unsigned int));
            ret = getSupportedMemClocks(dev, &cnt, clocks);
            if (ret == NVML_SUCCESS) {
                printf("      clocks: ");
                for (unsigned int j = 0; j < cnt; j++) printf("%u ", clocks[j]);
                printf("MHz\n");
            }
            free(clocks);
        }
    }

    /* Supported graphics clocks (at default mem clock) */
    nvmlDeviceGetSupportedGraphicsClocks_t getSupportedGfxClocks = 
        (nvmlDeviceGetSupportedGraphicsClocks_t)dlsym(lib, "nvmlDeviceGetSupportedGraphicsClocks");
    if (getSupportedGfxClocks) {
        unsigned int cnt = 0;
        ret = getSupportedGfxClocks(dev, 0, &cnt, NULL); /* memClk=0 */
        printf("[alt] nvmlDeviceGetSupportedGraphicsClocks(mem=0): ret=%u count=%u\n", ret, cnt);
    }

    /* Try some field value IDs for clock and power */
    nvmlDeviceGetFieldValues_t getFieldValues = 
        (nvmlDeviceGetFieldValues_t)dlsym(lib, "nvmlDeviceGetFieldValues");
    if (getFieldValues) {
        printf("\n[alt] Field Values scan for power/clock IDs:\n");
        /* Some potentially interesting field IDs */
        unsigned int fieldIds[] = {
            /* Known working: 187(pwrMin), 188(pwrMax), 189(pwrDefault), 230(recovery) */
            187, 188, 189, 230,
            /* Possible clock/power field IDs (from NVML headers) */
            10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
            50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60,
            80, 81, 82, 83, 84, 85,
            100, 101, 102, 103, 104, 105,
            120, 121, 122, 123, 124, 125, 126, 127, 128,
            130, 131, 132, 133, 134, 135,
            140, 141, 142, 143, 144, 145, 146, 147, 148, 149, 150,
            155, 156, 157, 158, 159, 160, 161, 162, 163, 164, 165,
            170, 171, 172, 173, 174, 175,
            180, 181, 182, 183, 184, 185, 186,
            190, 191, 192, 193, 194, 195, 196, 197, 198, 199, 200,
            210, 211, 212, 213, 214, 215, 220, 225, 226, 227, 228, 229,
            231, 232, 233, 234, 235, 236, 237, 238, 239, 240,
            250, 251, 252, 253, 254, 255
        };
        int nFields = sizeof(fieldIds) / sizeof(fieldIds[0]);
        for (int i = 0; i < nFields; i++) {
            nvmlFieldValue_t fv = {};
            fv.fieldId = fieldIds[i];
            ret = getFieldValues(dev, 1, &fv);
            if (fv.nvmlReturn == 0 && fv.valueType != 0) {
                printf("  ID %3u: type=%u ", fv.fieldId, fv.valueType);
                switch (fv.valueType) {
                    case 0: printf("ui=%u", fv.value.uiVal); break;
                    case 1: printf("ul=%llu", fv.value.ulVal); break;
                    case 2: printf("d=%f", fv.value.dVal); break;
                    case 3: printf("sll=%lld", fv.value.sllVal); break;
                    default: printf("raw"); break;
                }
                printf("\n");
            }
        }
    }

    shutdown();
    dlclose(lib);
    return 0;
}
