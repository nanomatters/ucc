/*
 * Detailed dump of PerformanceModes and MarginTemperature
 * Now that we know the correct version words.
 *
 * Build: gcc -O0 -g -o /tmp/nvml_detail /tmp/nvml_detail.c -ldl
 * Run:   /tmp/nvml_detail 2>&1
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

static void hexdump(const void *data, size_t len) {
    const uint8_t *p = data;
    for (size_t i = 0; i < len; i += 16) {
        printf("  %04zx: ", i);
        for (size_t j = 0; j < 16 && i + j < len; j++)
            printf("%02x ", p[i + j]);
        for (size_t j = 16 - ((len - i) < 16 ? (len - i) : 16); j > 0; j--)
            printf("   ");
        printf(" |");
        for (size_t j = 0; j < 16 && i + j < len; j++) {
            uint8_t c = p[i + j];
            printf("%c", (c >= 32 && c < 127) ? c : '.');
        }
        printf("|\n");
    }
}

int main(void) {
    void *lib = dlopen("libnvidia-ml.so.1", RTLD_NOW);
    if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }

    fn_init init = dlsym(lib, "nvmlInit_v2");
    fn_shutdown shut = dlsym(lib, "nvmlShutdown");
    fn_getHandle getH = dlsym(lib, "nvmlDeviceGetHandleByIndex_v2");
    fn_errStr errStr = dlsym(lib, "nvmlErrorString");

    if (!init || init() != 0) { fprintf(stderr, "init failed\n"); return 1; }

    nvmlDevice_t dev;
    if (!getH || getH(0, &dev) != 0) { fprintf(stderr, "getHandle failed\n"); return 1; }

    /* -- PerformanceModes -- */
    printf("=== PerformanceModes (sz=2052 ver=1) ===\n");
    {
        fn_dev_struct fn = dlsym(lib, "nvmlDeviceGetPerformanceModes");
        if (fn) {
            uint8_t buf[4096];
            memset(buf, 0, sizeof(buf));
            *(uint32_t *)buf = (2052 & 0x00FFFFFF) | (1 << 24);  /* 0x01000804 */
            nvmlReturn_t ret = fn(dev, buf);
            printf("ret=%d (%s)\n", ret, errStr(ret));
            if (ret == 0) {
                /* Find end of data */
                size_t last = 0;
                for (size_t i = 0; i < 2052; i++)
                    if (buf[i]) last = i;
                printf("Last nonzero byte at offset %zu\n", last);
                printf("\nHex dump (first 128 + around data):\n");
                hexdump(buf, last + 16 < 2052 ? last + 16 : 2052);

                /* Print as string starting at offset 4 */
                printf("\nAs text (offset 4):\n");
                buf[2051] = 0;
                printf("%s\n", (char *)(buf + 4));
            }
        }
    }

    /* -- MarginTemperature -- */
    printf("\n=== MarginTemperature (sz=8 ver=1) ===\n");
    {
        fn_dev_struct fn = dlsym(lib, "nvmlDeviceGetMarginTemperature");
        if (fn) {
            uint8_t buf[64];
            memset(buf, 0, sizeof(buf));
            *(uint32_t *)buf = (8 & 0x00FFFFFF) | (1 << 24);  /* 0x01000008 */
            nvmlReturn_t ret = fn(dev, buf);
            printf("ret=%d (%s)\n", ret, errStr(ret));
            if (ret == 0) {
                printf("Raw 16 bytes:\n");
                hexdump(buf, 16);
                printf("version word: 0x%08x\n", *(uint32_t *)buf);
                printf("value (offset 4, uint32): %u\n", *(uint32_t *)(buf + 4));
                printf("value (offset 4, int32):  %d\n", *(int32_t *)(buf + 4));
            }

            /* Try different sizes to see if there's more data */
            printf("\nTrying larger sizes:\n");
            for (uint32_t sz = 12; sz <= 64; sz += 4) {
                memset(buf, 0xAA, sizeof(buf));
                *(uint32_t *)buf = (sz & 0x00FFFFFF) | (1 << 24);
                ret = fn(dev, buf);
                if (ret == 0) {
                    size_t last = 0;
                    for (size_t i = 4; i < sz; i++)
                        if (buf[i] != 0xAA) last = i;
                    printf("  sz=%u: OK, last_written=%zu\n", sz, last);
                    hexdump(buf, last + 4 < 64 ? last + 4 : 64);
                }
            }
        }
    }

    /* -- CoolerInfo - try different num_coolers arguments -- */
    printf("\n=== CoolerInfo deeper probe ===\n");
    {
        /* Maybe it takes 3 args: device, index, struct */
        typedef nvmlReturn_t (*fn3)(nvmlDevice_t, unsigned int, void *);
        fn3 fn = (fn3)dlsym(lib, "nvmlDeviceGetCoolerInfo");
        if (fn) {
            for (unsigned int idx = 0; idx < 4; idx++) {
                for (uint32_t sz = 8; sz <= 256; sz += 4) {
                    uint8_t buf[512];
                    memset(buf, 0, sizeof(buf));
                    *(uint32_t *)buf = (sz & 0x00FFFFFF) | (1 << 24);
                    nvmlReturn_t ret = fn(dev, idx, buf);
                    if (ret == 0) {
                        printf("  3-arg: idx=%u sz=%u => OK!\n", idx, sz);
                        hexdump(buf, 64);
                    } else if (ret != 25 && ret != 3 && ret != 15) {
                        printf("  3-arg: idx=%u sz=%u => %d (%s)\n",
                               idx, sz, ret, errStr(ret));
                    }
                }
            }
        }
    }

    /* -- ThermalSettings deeper look -- */
    printf("\n=== ThermalSettings (known working, deep field map) ===\n");
    {
        fn_dev_struct fn = dlsym(lib, "nvmlDeviceGetThermalSettings");
        if (fn) {
            /* Try with larger struct to find real data */
            uint8_t buf[4096];
            memset(buf, 0xCC, sizeof(buf));
            *(uint32_t *)buf = (256 & 0x00FFFFFF) | (1 << 24);
            nvmlReturn_t ret = fn(dev, buf);
            printf("sz=256 v=1 ret=%d\n", ret);
            if (ret == 0) {
                size_t last = 0;
                for (size_t i = 4; i < 256; i++)
                    if (buf[i] != 0xCC) last = i;
                printf("Last written offset: %zu\n", last);
                hexdump(buf, last + 8);
            }

            /* Try with sensor_index as 2nd arg before struct */
            typedef nvmlReturn_t (*fn3)(nvmlDevice_t, unsigned int, void *);
            fn3 fn3p = (fn3)fn;
            for (unsigned int idx = 0; idx < 8; idx++) {
                memset(buf, 0xCC, sizeof(buf));
                *(uint32_t *)buf = (256 & 0x00FFFFFF) | (1 << 24);
                ret = fn3p(dev, idx, buf);
                if (ret == 0) {
                    size_t last = 0;
                    for (size_t i = 4; i < 256; i++)
                        if (buf[i] != 0xCC) last = i;
                    printf("  3-arg idx=%u: OK last=%zu\n", idx, last);
                    hexdump(buf, last + 8);
                }
            }
        }
    }

    /* -- DynamicPstatesInfo: repeat with full hexdump -- */
    printf("\n=== DynamicPstatesInfo full dump ===\n");
    {
        fn_dev_struct fn = dlsym(lib, "nvmlDeviceGetDynamicPstatesInfo");
        if (fn) {
            uint8_t buf[256];
            memset(buf, 0xCC, sizeof(buf));
            nvmlReturn_t ret = fn(dev, buf);
            printf("ret=%d\n", ret);
            if (ret == 0) {
                size_t last = 0;
                for (size_t i = 0; i < 256; i++)
                    if (buf[i] != 0xCC) last = i;
                printf("Last written: %zu\n", last);
                hexdump(buf, last + 4);
            }
        }
    }

    /* -- PowerMizerMode_v1 full dump -- */
    printf("\n=== PowerMizerMode_v1 full dump ===\n");
    {
        fn_dev_struct fn = dlsym(lib, "nvmlDeviceGetPowerMizerMode_v1");
        if (fn) {
            uint8_t buf[256];
            memset(buf, 0xCC, sizeof(buf));
            nvmlReturn_t ret = fn(dev, buf);
            printf("ret=%d\n", ret);
            if (ret == 0) {
                size_t last = 0;
                for (size_t i = 0; i < 256; i++)
                    if (buf[i] != 0xCC) last = i;
                printf("Last written: %zu\n", last);
                hexdump(buf, last + 4);
            }
        }
    }

    shut();
    dlclose(lib);
    return 0;
}
