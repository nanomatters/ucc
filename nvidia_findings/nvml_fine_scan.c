/*
 * Fine-grained NVML struct version scanner
 * Tries byte-level granularity and higher version numbers
 * Also tries different version encodings.
 *
 * Build: gcc -O0 -g -o /tmp/nvml_fine_scan /tmp/nvml_fine_scan.c -ldl
 * Run:   /tmp/nvml_fine_scan 2>&1
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <stdint.h>

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

static inline uint32_t make_ver(uint32_t size, uint32_t vnum) {
    return (size & 0x00FFFFFF) | (vnum << 24);
}

/*
 * Scan all sizes from min_sz to max_sz with step,
 * and version numbers from 1 to max_ver.
 * Also try raw version numbers (not encoding size).
 */
static void scan_function(const char *name, fn_dev_struct fn,
                          uint32_t min_sz, uint32_t max_sz, uint32_t step,
                          int max_ver) {
    uint8_t buf[65536];
    printf("\n=== Scanning %s (size %u-%u step %u, ver 1-%d) ===\n",
           name, min_sz, max_sz, step, max_ver);

    int found = 0;
    for (uint32_t sz = min_sz; sz <= max_sz; sz += step) {
        for (int v = 1; v <= max_ver; v++) {
            uint32_t ver = make_ver(sz, v);
            memset(buf, 0, sizeof(buf));
            *(uint32_t *)buf = ver;
            nvmlReturn_t ret = fn(dev, buf);
            if (ret == 0) {
                printf("  >> SUCCESS: size=%u, ver=%u, version_field=0x%08x\n", sz, v, ver);
                /* Find last non-zero byte */
                size_t last = 0;
                for (size_t i = 4; i < sz && i < 65536; i++) {
                    if (buf[i]) last = i;
                }
                printf("     Last non-zero byte at offset %zu\n", last);
                /* Dump first 128 bytes */
                printf("     Hex: ");
                for (size_t i = 0; i < 128 && i < sz; i++) {
                    printf("%02x", buf[i]);
                    if (i % 4 == 3) printf(" ");
                    if (i % 32 == 31) printf("\n          ");
                }
                printf("\n");
                found = 1;
            } else if (ret != 25 && ret != -9 && ret != 15) {
                /* Interesting: version accepted but different error */
                printf("  !! size=%u, ver=%u => ret=%d (%s)\n",
                       sz, v, ret, errString ? errString(ret) : "?");
            }
        }
    }

    /* Also try raw version numbers (maybe it's just a simple version ID, not size+ver) */
    printf("  Trying raw version numbers (not size-encoded)...\n");
    for (uint32_t raw_ver = 1; raw_ver <= 20; raw_ver++) {
        memset(buf, 0, sizeof(buf));
        *(uint32_t *)buf = raw_ver;
        nvmlReturn_t ret = fn(dev, buf);
        if (ret == 0) {
            printf("  >> SUCCESS with raw version %u!\n", raw_ver);
            found = 1;
        } else if (ret != 25 && ret != -9 && ret != 15) {
            printf("  !! raw_ver=%u => ret=%d (%s)\n",
                   raw_ver, ret, errString ? errString(ret) : "?");
        }
    }

    /* Try very large raw version values that look like NVML defines */
    static const uint32_t special_versions[] = {
        0x01000040, 0x01000080, 0x01000100, 0x01000200, 0x01000400,
        0x02000040, 0x02000080, 0x02000100, 0x02000200, 0x02000400,
        0x01000334, 0x02000334, 0x0100020c, 0x020002a0,
        /* Try versions with sizes that hit odd values */
        0x01000022, 0x01000026, 0x0100002a, 0x0100002e,
        0x01000032, 0x01000046, 0x0100004a, 0x0100004e,
        0
    };
    printf("  Trying special version values...\n");
    for (int i = 0; special_versions[i]; i++) {
        memset(buf, 0, sizeof(buf));
        *(uint32_t *)buf = special_versions[i];
        nvmlReturn_t ret = fn(dev, buf);
        if (ret == 0) {
            printf("  >> SUCCESS with version 0x%08x!\n", special_versions[i]);
            found = 1;
        } else if (ret != 25 && ret != -9 && ret != 15) {
            printf("  !! ver=0x%08x => ret=%d (%s)\n",
                   special_versions[i], ret, errString ? errString(ret) : "?");
        }
    }

    if (!found) printf("  No working version found.\n");
}

/* Try calling functions without any version header (raw data) */
static void scan_no_version(const char *name, fn_dev_struct fn) {
    uint8_t buf[65536];
    printf("\n=== Trying %s without version header ===\n", name);

    memset(buf, 0, sizeof(buf));
    nvmlReturn_t ret = fn(dev, buf);
    printf("  Zero buffer: ret=%d (%s)\n", ret, errString ? errString(ret) : "?");

    memset(buf, 0xFF, sizeof(buf));
    *(uint32_t *)buf = 0;
    ret = fn(dev, buf);
    printf("  0xFF buffer (ver=0): ret=%d (%s)\n", ret, errString ? errString(ret) : "?");
}

/* Probe function with 3-arg signature: fn(device, sensor_type, &struct) */
typedef nvmlReturn_t (*fn_dev_int_struct)(nvmlDevice_t, unsigned int, void *);
static void scan_3arg(const char *name, fn_dev_int_struct fn,
                      int max_sensor, uint32_t min_sz, uint32_t max_sz, uint32_t step, int max_ver) {
    uint8_t buf[65536];
    printf("\n=== Scanning 3-arg %s ===\n", name);
    for (int sensor = 0; sensor <= max_sensor; sensor++) {
        for (uint32_t sz = min_sz; sz <= max_sz; sz += step) {
            for (int v = 1; v <= max_ver; v++) {
                uint32_t ver = make_ver(sz, v);
                memset(buf, 0, sizeof(buf));
                *(uint32_t *)buf = ver;
                nvmlReturn_t ret = fn(dev, sensor, buf);
                if (ret == 0) {
                    printf("  >> SUCCESS: sensor=%d, size=%u, ver=%u (0x%08x)\n",
                           sensor, sz, v, ver);
                    size_t last = 0;
                    for (size_t i = 4; i < sz; i++) { if (buf[i]) last = i; }
                    printf("     Last non-zero at %zu\n", last);
                    printf("     Hex: ");
                    for (size_t i = 0; i < 128 && i < sz; i++) {
                        printf("%02x", buf[i]);
                        if (i % 4 == 3) printf(" ");
                        if (i % 32 == 31) printf("\n          ");
                    }
                    printf("\n");
                } else if (ret != 25 && ret != -9 && ret != 15) {
                    printf("  sensor=%d size=%u ver=%u => ret=%d (%s)\n",
                           sensor, sz, v, ret, errString ? errString(ret) : "?");
                }
            }
        }
    }
}

int main(void) {
    lib = dlopen("libnvidia-ml.so.1", RTLD_NOW);
    if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }

    fn_init init = dlsym(lib, "nvmlInit_v2");
    fn_shutdown shut = dlsym(lib, "nvmlShutdown");
    fn_getHandle getH = dlsym(lib, "nvmlDeviceGetHandleByIndex_v2");
    errString = dlsym(lib, "nvmlErrorString");

    if (init() != 0) { fprintf(stderr, "init failed\n"); return 1; }
    if (getH(0, &dev) != 0) { fprintf(stderr, "getHandle failed\n"); return 1; }

    printf("=== NVML Fine-Grained Struct Version Scanner ===\n");

    /* CoolerInfo: try sizes 2-8192 step 2, versions 1-10 */
    fn_dev_struct coolerInfo = dlsym(lib, "nvmlDeviceGetCoolerInfo");
    if (coolerInfo) {
        scan_function("CoolerInfo", coolerInfo, 4, 8192, 2, 10);
        scan_no_version("CoolerInfo", coolerInfo);
    }

    /* PerformanceModes - same treatment */
    fn_dev_struct perfModes = dlsym(lib, "nvmlDeviceGetPerformanceModes");
    if (perfModes) {
        scan_function("PerformanceModes", perfModes, 4, 16384, 4, 10);
        scan_no_version("PerformanceModes", perfModes);
    }

    /* MarginTemperature */
    fn_dev_struct marginTemp = dlsym(lib, "nvmlDeviceGetMarginTemperature");
    if (marginTemp) {
        /* MarginTemp might be fn(device, sensorType, &struct) */
        scan_function("MarginTemperature (2-arg)", marginTemp, 4, 1024, 2, 10);
        scan_no_version("MarginTemperature", marginTemp);
        /* Try as 3-arg: fn(device, sensor, &struct) */
        scan_3arg("MarginTemperature (3-arg)", (fn_dev_int_struct)marginTemp,
                  5, 4, 256, 4, 5);
    }

    /* WorkloadPowerProfile - the UpdateProfiles_v1 returned Invalid Argument everywhere
       meaning version was accepted but arguments were wrong. Let's try with non-zero data */
    fn_dev_struct wpUpdate = dlsym(lib, "nvmlDeviceWorkloadPowerProfileUpdateProfiles_v1");
    if (wpUpdate) {
        printf("\n=== WorkloadPowerProfileUpdateProfiles_v1 with non-zero data ===\n");
        /* This function returns "Invalid Argument" for ALL size/ver combos
         * meaning it doesn't check version at all, but our data is wrong.
         * Maybe it needs specific fields set. Try with count=1 etc. */
        uint8_t buf[4096];
        for (uint32_t raw = 0; raw <= 10; raw++) {
            memset(buf, 0, sizeof(buf));
            *(uint32_t *)buf = raw;  /* Maybe first field is a count or flags */
            nvmlReturn_t ret = wpUpdate(dev, buf);
            if (ret == 0) {
                printf("  SUCCESS with first_field=%u\n", raw);
            } else {
                printf("  first_field=%u => ret=%d (%s)\n", raw, ret, errString ? errString(ret) : "?");
            }
        }
    }

    /* WorkloadPowerProfile - GetProfilesInfo and GetCurrentProfiles */
    fn_dev_struct wpGetInfo = dlsym(lib, "nvmlDeviceWorkloadPowerProfileGetProfilesInfo");
    fn_dev_struct wpGetCur = dlsym(lib, "nvmlDeviceWorkloadPowerProfileGetCurrentProfiles");
    if (wpGetInfo) scan_function("WPProfileGetProfilesInfo", wpGetInfo, 4, 16384, 4, 10);
    if (wpGetCur) scan_function("WPProfileGetCurrentProfiles", wpGetCur, 4, 4096, 4, 10);

    /* PowerSmoothing functions */
    fn_dev_struct psActivate = dlsym(lib, "nvmlDevicePowerSmoothingActivatePresetProfile");
    fn_dev_struct psUpdate = dlsym(lib, "nvmlDevicePowerSmoothingUpdatePresetProfileParam");
    fn_dev_struct psSetState = dlsym(lib, "nvmlDevicePowerSmoothingSetState");

    if (psActivate) scan_function("PSmoothing_Activate", psActivate, 4, 4096, 4, 10);
    if (psUpdate) scan_function("PSmoothing_Update", psUpdate, 4, 4096, 4, 10);
    if (psSetState) scan_function("PSmoothing_SetState", psSetState, 4, 4096, 4, 10);

    shut();
    dlclose(lib);
    printf("\n=== SCAN COMPLETE ===\n");
    return 0;
}
