/*
 * field_scan_595.c - NVML field-value interface scanner
 *
 * Goal:
 * - Probe nvmlDeviceGetFieldValues directly (the interface used by nvidia-smi)
 * - Discover which field IDs return live data on this driver/GPU
 * - Correlate candidate IDs for clocks, temperature, power, and limiter data
 *
 * Build: gcc -O2 -Wall -Wextra -o field_scan_595 field_scan_595.c -ldl
 * Run:   ./field_scan_595 | tee field_scan_595_output.txt
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void *nvmlDevice_t;
typedef int nvmlReturn_t;

typedef nvmlReturn_t (*nvmlInit_t)(void);
typedef nvmlReturn_t (*nvmlShutdown_t)(void);
typedef nvmlReturn_t (*nvmlDeviceGetHandleByIndex_t)(unsigned int, nvmlDevice_t *);
typedef const char *(*nvmlErrorString_t)(nvmlReturn_t);
typedef nvmlReturn_t (*nvmlDeviceGetTemperature_t)(nvmlDevice_t, unsigned int, unsigned int *);
typedef nvmlReturn_t (*nvmlDeviceGetPowerUsage_t)(nvmlDevice_t, unsigned int *);
typedef nvmlReturn_t (*nvmlDeviceGetClockInfo_t)(nvmlDevice_t, unsigned int, unsigned int *);
typedef nvmlReturn_t (*nvmlDeviceGetFieldValues_t)(nvmlDevice_t, int, void *);

enum {
    NVML_SUCCESS = 0,
    NVML_ERROR_NOT_SUPPORTED = 3,
    NVML_TEMPERATURE_GPU = 0,
    NVML_CLOCK_GRAPHICS = 0,
    NVML_CLOCK_SM = 1,
    NVML_CLOCK_MEM = 2,
    NVML_CLOCK_VIDEO = 3,
};

/*
 * Matches the documented NVML field value layout used by nvmlDeviceGetFieldValues.
 * String storage is oversized intentionally for safety on unknown driver variants.
 */
typedef struct {
    unsigned int fieldId;
    unsigned int scopeId;
    long long timestamp;
    long long latencyUsec;
    unsigned int valueType;
    nvmlReturn_t nvmlReturn;
    union {
        unsigned int uiVal;
        unsigned long ulVal;
        unsigned long long ullVal;
        long long sllVal;
        double dVal;
        char strVal[4096];
    } value;
} fieldValue_t;

static int is_probably_ascii(const char *s, size_t max_len) {
    size_t n = 0;
    for (; n < max_len && s[n] != '\0'; ++n) {
        unsigned char c = (unsigned char)s[n];
        if (c < 32 || c > 126) return 0;
    }
    return n > 0;
}

static void print_value(const fieldValue_t *f) {
    printf("type=%u ret=%d ts=%lld lat=%lld ",
           f->valueType, f->nvmlReturn, f->timestamp, f->latencyUsec);

    switch (f->valueType) {
        case 0: /* unsigned int */
            printf("ui=%u", f->value.uiVal);
            break;
        case 1: /* unsigned long */
            printf("ul=%lu", f->value.ulVal);
            break;
        case 2: /* unsigned long long */
            printf("ull=%llu", (unsigned long long)f->value.ullVal);
            break;
        case 3: /* signed long long */
            printf("sll=%lld", (long long)f->value.sllVal);
            break;
        case 4: /* double */
            printf("d=%f", f->value.dVal);
            break;
        case 5: /* string */
            if (is_probably_ascii(f->value.strVal, sizeof(f->value.strVal)))
                printf("str=\"%s\"", f->value.strVal);
            else
                printf("str=<non-ascii>");
            break;
        default:
            /* Print multiple interpretations for unknown value types. */
            printf("raw(ui=%u ull=%llu sll=%lld)",
                   f->value.uiVal,
                   (unsigned long long)f->value.ullVal,
                   (long long)f->value.sllVal);
            break;
    }
}

int main(int argc, char **argv) {
    void *lib = dlopen("libnvidia-ml.so.1", RTLD_LAZY);
    if (!lib) {
        fprintf(stderr, "Failed to load libnvidia-ml.so.1\n");
        return 1;
    }

    nvmlInit_t nvmlInit = (nvmlInit_t)dlsym(lib, "nvmlInit_v2");
    nvmlShutdown_t nvmlShutdown = (nvmlShutdown_t)dlsym(lib, "nvmlShutdown");
    nvmlDeviceGetHandleByIndex_t getHandle =
        (nvmlDeviceGetHandleByIndex_t)dlsym(lib, "nvmlDeviceGetHandleByIndex_v2");
    nvmlErrorString_t errStr = (nvmlErrorString_t)dlsym(lib, "nvmlErrorString");
    nvmlDeviceGetTemperature_t getTemp =
        (nvmlDeviceGetTemperature_t)dlsym(lib, "nvmlDeviceGetTemperature");
    nvmlDeviceGetPowerUsage_t getPower =
        (nvmlDeviceGetPowerUsage_t)dlsym(lib, "nvmlDeviceGetPowerUsage");
    nvmlDeviceGetClockInfo_t getClock =
        (nvmlDeviceGetClockInfo_t)dlsym(lib, "nvmlDeviceGetClockInfo");
    nvmlDeviceGetFieldValues_t getFields =
        (nvmlDeviceGetFieldValues_t)dlsym(lib, "nvmlDeviceGetFieldValues");

    if (!nvmlInit || !nvmlShutdown || !getHandle || !getFields) {
        fprintf(stderr, "Missing required NVML symbols\n");
        return 1;
    }

    nvmlReturn_t ret = nvmlInit();
    if (ret != NVML_SUCCESS) {
        fprintf(stderr, "nvmlInit_v2 failed: %d (%s)\n", ret, errStr ? errStr(ret) : "?");
        return 1;
    }

    nvmlDevice_t dev = NULL;
    ret = getHandle(0, &dev);
    if (ret != NVML_SUCCESS) {
        fprintf(stderr, "nvmlDeviceGetHandleByIndex_v2 failed: %d (%s)\n", ret, errStr ? errStr(ret) : "?");
        nvmlShutdown();
        return 1;
    }

    unsigned int maxId = 4096;
    if (argc > 1) {
        unsigned long parsed = strtoul(argv[1], NULL, 10);
        if (parsed >= 1 && parsed <= 65535)
            maxId = (unsigned int)parsed;
    }

    printf("============================================================\n");
    printf("NVML Field Scan - Driver 595 - direct nvmlDeviceGetFieldValues\n");
    printf("============================================================\n\n");

    if (getTemp || getPower || getClock) {
        unsigned int temp = 0, powerMw = 0;
        unsigned int g = 0, sm = 0, mem = 0, vid = 0;
        int hasTemp = getTemp && (getTemp(dev, NVML_TEMPERATURE_GPU, &temp) == NVML_SUCCESS);
        int hasPower = getPower && (getPower(dev, &powerMw) == NVML_SUCCESS);
        int hasG = getClock && (getClock(dev, NVML_CLOCK_GRAPHICS, &g) == NVML_SUCCESS);
        int hasSm = getClock && (getClock(dev, NVML_CLOCK_SM, &sm) == NVML_SUCCESS);
        int hasMem = getClock && (getClock(dev, NVML_CLOCK_MEM, &mem) == NVML_SUCCESS);
        int hasVid = getClock && (getClock(dev, NVML_CLOCK_VIDEO, &vid) == NVML_SUCCESS);

        printf("Reference NVML values (for correlation):\n");
        if (hasTemp) printf("  temp=%u C\n", temp);
        if (hasPower) printf("  power=%u mW\n", powerMw);
        if (hasG || hasSm || hasMem || hasVid) {
            printf("  clocks MHz: gr=%u sm=%u mem=%u video=%u\n",
                   hasG ? g : 0, hasSm ? sm : 0, hasMem ? mem : 0, hasVid ? vid : 0);
        }
        printf("\n");
    }

    printf("Phase 1: broad scan IDs 1..%u (scopeId=0)\n", maxId);
    printf("Only IDs returning data (nvmlReturn=0) are listed.\n\n");

    unsigned int hits = 0;
    for (unsigned int id = 1; id <= maxId; ++id) {
        fieldValue_t f;
        memset(&f, 0, sizeof(f));
        f.fieldId = id;
        f.scopeId = 0;

        ret = getFields(dev, 1, &f);
        if (ret != NVML_SUCCESS && ret != NVML_ERROR_NOT_SUPPORTED) {
            printf("ID %4u -> call ret=%d (%s)\n", id, ret, errStr ? errStr(ret) : "?");
            continue;
        }

        if (f.nvmlReturn == NVML_SUCCESS) {
            ++hits;
            printf("ID %4u -> ", id);
            print_value(&f);
            printf("\n");
        }
    }

    printf("\nPhase 1 summary: %u field IDs returned successful data.\n\n", hits);

    printf("Phase 2: candidate correlation for temperature/power/clock-like values\n");
    printf("Heuristic: print successful numeric fields with plausible ranges.\n\n");

    for (unsigned int id = 1; id <= maxId; ++id) {
        fieldValue_t f;
        memset(&f, 0, sizeof(f));
        f.fieldId = id;
        f.scopeId = 0;

        ret = getFields(dev, 1, &f);
        if (ret != NVML_SUCCESS || f.nvmlReturn != NVML_SUCCESS)
            continue;

        long long v = 0;
        int has_numeric = 1;
        switch (f.valueType) {
            case 0: v = (long long)f.value.uiVal; break;
            case 1: v = (long long)f.value.ulVal; break;
            case 2: v = (long long)f.value.ullVal; break;
            case 3: v = (long long)f.value.sllVal; break;
            default: has_numeric = 0; break;
        }
        if (!has_numeric) continue;

        int plausible_temp = (v >= 10 && v <= 120);
        int plausible_clock = (v >= 100 && v <= 4000);
        int plausible_power_mw = (v >= 10000 && v <= 1000000);
        int plausible_time_us = (v >= 1000 && v <= 1000000000000LL);
        int plausible_uv = (v >= 500000 && v <= 2000000);

        if (plausible_temp || plausible_clock || plausible_power_mw || plausible_time_us || plausible_uv) {
            printf("ID %4u -> ", id);
            print_value(&f);
            if (plausible_temp) printf("  [temp-like]");
            if (plausible_clock) printf("  [clock-like]");
            if (plausible_power_mw) printf("  [power-like]");
            if (plausible_time_us) printf("  [counter-like]");
            if (plausible_uv) printf("  [voltage-like]");
            printf("\n");
        }
    }

    printf("\nPhase 3: probe successful IDs across scopeId=0..8\n\n");
    {
        unsigned int probeIds[512];
        unsigned int probeCount = 0;

        for (unsigned int id = 1; id <= maxId && probeCount < 512; ++id) {
            fieldValue_t f;
            memset(&f, 0, sizeof(f));
            f.fieldId = id;
            f.scopeId = 0;
            ret = getFields(dev, 1, &f);
            if (ret == NVML_SUCCESS && f.nvmlReturn == NVML_SUCCESS)
                probeIds[probeCount++] = id;
        }

        for (unsigned int i = 0; i < probeCount; ++i) {
            unsigned int id = probeIds[i];
            int printedHeader = 0;
            for (unsigned int scope = 0; scope <= 8; ++scope) {
                fieldValue_t f;
                memset(&f, 0, sizeof(f));
                f.fieldId = id;
                f.scopeId = scope;
                ret = getFields(dev, 1, &f);
                if (ret == NVML_SUCCESS && f.nvmlReturn == NVML_SUCCESS) {
                    if (!printedHeader) {
                        printf("ID %4u:\n", id);
                        printedHeader = 1;
                    }
                    printf("  scope=%u -> ", scope);
                    print_value(&f);
                    if (id == 196 && f.valueType == 5) {
                        const unsigned char *b = (const unsigned char *)f.value.strVal;
                        printf(" hex=");
                        for (int k = 0; k < 16; ++k)
                            printf("%02x", b[k]);
                    }
                    printf("\n");
                }
            }
        }
    }

    nvmlShutdown();
    return 0;
}
