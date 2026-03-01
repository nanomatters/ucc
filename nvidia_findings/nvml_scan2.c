/*
 * Safe NVML struct version scanner v2
 * Uses a large buffer but varies the version field systematically.
 * Uses signal handlers to catch crashes.
 *
 * Build: gcc -O0 -g -o /tmp/nvml_scan2 /tmp/nvml_scan2.c -ldl
 * Run:   /tmp/nvml_scan2 2>&1
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <stdint.h>
#include <signal.h>
#include <setjmp.h>
#include <unistd.h>

typedef void *nvmlDevice_t;
typedef int nvmlReturn_t;
typedef nvmlReturn_t (*fn_init)(void);
typedef nvmlReturn_t (*fn_shutdown)(void);
typedef nvmlReturn_t (*fn_getHandle)(unsigned int, nvmlDevice_t *);
typedef nvmlReturn_t (*fn_dev_struct)(nvmlDevice_t, void *);
typedef const char *(*fn_errStr)(nvmlReturn_t);

static void *lib;
static nvmlDevice_t dev;
static fn_errStr errString;
static jmp_buf jmp_env;
static volatile int in_probe = 0;

static void segv_handler(int sig) {
    if (in_probe) {
        longjmp(jmp_env, 1);
    }
    _exit(128 + sig);
}

static inline uint32_t make_ver(uint32_t size, uint32_t vnum) {
    return (size & 0x00FFFFFF) | (vnum << 24);
}

static uint8_t big_buf[131072] __attribute__((aligned(4096)));

static void hexdump_line(const void *data, size_t bytes) {
    const uint8_t *p = data;
    for (size_t i = 0; i < bytes; i++) {
        printf("%02x", p[i]);
        if (i % 4 == 3) printf(" ");
    }
    printf("\n");
}

static void safe_scan(const char *name, fn_dev_struct fn,
                      uint32_t min_sz, uint32_t max_sz, uint32_t step,
                      int max_ver) {
    printf("\n=== %s ===\n", name);
    fflush(stdout);

    int found = 0;
    for (uint32_t sz = min_sz; sz <= max_sz; sz += step) {
        for (int v = 1; v <= max_ver; v++) {
            uint32_t ver = make_ver(sz, v);
            memset(big_buf, 0, sizeof(big_buf));
            *(uint32_t *)big_buf = ver;

            in_probe = 1;
            if (setjmp(jmp_env) == 0) {
                nvmlReturn_t ret = fn(dev, big_buf);
                in_probe = 0;

                if (ret == 0) {
                    size_t last = 0;
                    for (size_t i = 4; i < sz && i < sizeof(big_buf); i++) {
                        if (big_buf[i]) last = i;
                    }
                    printf("  OK: sz=%u ver=%d (0x%08x) last_nonzero=%zu\n",
                           sz, v, ver, last);
                    /* Print first 64 bytes */
                    printf("    ");
                    hexdump_line(big_buf, 64 < sz ? 64 : sz);
                    found = 1;
                } else if (ret != 25 && ret != -9 && ret != 15) {
                    printf("  sz=%u v=%d => %d (%s)\n", sz, v, ret,
                           errString ? errString(ret) : "?");
                }
            } else {
                in_probe = 0;
                printf("  CRASH at sz=%u v=%d, skipping\n", sz, v);
            }
            fflush(stdout);
        }
    }
    if (!found) printf("  No working version found\n");
    fflush(stdout);
}

int main(void) {
    signal(SIGSEGV, segv_handler);
    signal(SIGBUS, segv_handler);

    lib = dlopen("libnvidia-ml.so.1", RTLD_NOW);
    if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }

    fn_init init = dlsym(lib, "nvmlInit_v2");
    fn_shutdown shut = dlsym(lib, "nvmlShutdown");
    fn_getHandle getH = dlsym(lib, "nvmlDeviceGetHandleByIndex_v2");
    errString = dlsym(lib, "nvmlErrorString");

    if (!init || init() != 0) { fprintf(stderr, "init failed\n"); return 1; }
    if (!getH || getH(0, &dev) != 0) { fprintf(stderr, "getHandle failed\n"); return 1; }

    printf("=== NVML Safe Version Scanner v2 ===\n");
    fflush(stdout);

    /* CoolerInfo: scan sizes 4-65536 step 4, versions 1-10 */
    fn_dev_struct fn;
    fn = dlsym(lib, "nvmlDeviceGetCoolerInfo");
    if (fn) safe_scan("CoolerInfo", fn, 4, 65536, 4, 10);

    /* PerformanceModes */
    fn = dlsym(lib, "nvmlDeviceGetPerformanceModes");
    if (fn) safe_scan("PerformanceModes", fn, 4, 65536, 4, 10);

    /* MarginTemperature */
    fn = dlsym(lib, "nvmlDeviceGetMarginTemperature");
    if (fn) safe_scan("MarginTemperature", fn, 4, 65536, 4, 10);

    /* WorkloadPowerProfile GetProfilesInfo */
    fn = dlsym(lib, "nvmlDeviceWorkloadPowerProfileGetProfilesInfo");
    if (fn) safe_scan("WP_GetProfilesInfo", fn, 4, 65536, 4, 10);

    /* WorkloadPowerProfile GetCurrentProfiles */
    fn = dlsym(lib, "nvmlDeviceWorkloadPowerProfileGetCurrentProfiles");
    if (fn) safe_scan("WP_GetCurrentProfiles", fn, 4, 65536, 4, 10);

    /* PowerSmoothing */
    fn = dlsym(lib, "nvmlDevicePowerSmoothingActivatePresetProfile");
    if (fn) safe_scan("PS_Activate", fn, 4, 16384, 4, 10);

    fn = dlsym(lib, "nvmlDevicePowerSmoothingUpdatePresetProfileParam");
    if (fn) safe_scan("PS_Update", fn, 4, 16384, 4, 10);

    fn = dlsym(lib, "nvmlDevicePowerSmoothingSetState");
    if (fn) safe_scan("PS_SetState", fn, 4, 16384, 4, 10);

    shut();
    dlclose(lib);
    printf("\n=== SCAN COMPLETE ===\n");
    return 0;
}
