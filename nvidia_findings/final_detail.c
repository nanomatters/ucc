/*
 * final_detail.c - Get detailed data for remaining unknowns
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

typedef void* NvPhysicalGpuHandle;
typedef int NvAPI_Status;
typedef void* (*QueryInterfaceFn)(unsigned int);

#define NVAPI_INITIALIZE       0x0150E828
#define NVAPI_ENUM_GPUS        0xE5AC921F

int main(void) {
    void *lib = dlopen("libnvidia-api.so.1", RTLD_NOW);
    if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    QueryInterfaceFn qi = dlsym(lib, "nvapi_QueryInterface");
    if (!qi) { fprintf(stderr, "no qi\n"); return 1; }

    typedef NvAPI_Status (*InitFn)(void);
    InitFn init = (InitFn)qi(NVAPI_INITIALIZE);
    if (!init || init() != 0) { fprintf(stderr, "init failed\n"); return 1; }

    typedef NvAPI_Status (*EnumGpusFn)(NvPhysicalGpuHandle*, unsigned int*);
    EnumGpusFn enumGpus = (EnumGpusFn)qi(NVAPI_ENUM_GPUS);
    NvPhysicalGpuHandle handles[64];
    unsigned int count = 0;
    enumGpus(handles, &count);
    NvPhysicalGpuHandle gpu = handles[0];

    typedef NvAPI_Status (*NvGpuFn)(NvPhysicalGpuHandle, void*);

    /* Unknown_0x0D258BB5 v1 (sz=88) and v2 (sz=104) */
    printf("=== Unknown_0x0D258BB5 ===\n");
    {
        NvGpuFn func = (NvGpuFn)qi(0x0D258BB5);
        if (func) {
            /* v1 sz=88 */
            unsigned char buf1[256] = {0};
            *(unsigned int*)buf1 = (88 & 0xFFFF) | (1 << 16);
            NvAPI_Status s = func(gpu, buf1);
            printf("v1 sz=88 status=%d\n", s);
            if (s == 0) {
                printf("  Full dump (u32[22]):\n  ");
                unsigned int *p = (unsigned int*)buf1;
                for (int i = 0; i < 22; i++) printf("[%2d]=%10u (0x%08x)  ", i, p[i], p[i]);
                printf("\n");
            }

            /* v2 sz=104 */
            unsigned char buf2[256] = {0};
            *(unsigned int*)buf2 = (104 & 0xFFFF) | (2 << 16);
            s = func(gpu, buf2);
            printf("v2 sz=104 status=%d\n", s);
            if (s == 0) {
                printf("  Full dump (u32[26]):\n  ");
                unsigned int *p = (unsigned int*)buf2;
                for (int i = 0; i < 26; i++) printf("[%2d]=%10u (0x%08x)  ", i, p[i], p[i]);
                printf("\n");
                /* Show the extra fields in v2 vs v1 */
                printf("  Extra v2 fields (byte 88-103):\n  ");
                for (int i = 88; i < 104; i++) printf("%02x ", buf2[i]);
                printf("\n  As u32: ");
                unsigned int *extra = (unsigned int*)(buf2 + 88);
                for (int i = 0; i < 4; i++) printf("[%d]=%u (0x%08x) ", i, extra[i], extra[i]);
                printf("\n");
            }
        }
    }

    /* GetCurrentPstate deeper look - what's in the data? */
    printf("\n=== GetCurrentPstate (0x927DA4F6) ===\n");
    {
        NvGpuFn func = (NvGpuFn)qi(0x927DA4F6);
        if (func) {
            unsigned char buf[256] = {0};
            *(unsigned int*)buf = (32 & 0xFFFF) | (1 << 16);
            NvAPI_Status s = func(gpu, buf);
            printf("v1 sz=32 status=%d\n", s);
            printf("  As u32[8]: ");
            unsigned int *p = (unsigned int*)buf;
            for (int i = 0; i < 8; i++) printf("[%d]=%u ", i, p[i]);
            printf("\n");
        }
    }

    /* Re-check AllClockFrequencies with type parameter in v3 */
    printf("\n=== AllClockFrequencies v3 (0xDCB616C3) - Detailed ===\n");
    {
        NvGpuFn func = (NvGpuFn)qi(0xDCB616C3);
        if (func) {
            /* ClockType values: 0=CURRENT, 1=BASE, 2=BOOST, 3=TDP */
            for (int ct = 0; ct <= 3; ct++) {
                unsigned char buf[512] = {0};
                *(unsigned int*)buf = (264 & 0xFFFF) | (3 << 16);
                *(unsigned int*)(buf + 4) = ct;  /* clock type */
                NvAPI_Status s = func(gpu, buf);
                const char *ct_name[] = {"CURRENT", "BASE", "BOOST", "TDP"};
                printf("ClockType=%s status=%d\n", ct_name[ct], s);
                if (s == 0) {
                    /* domains start at offset 8, 32 bytes each, 8 domains */
                    printf("  Domain clocks (MHz):\n");
                    for (int d = 0; d < 8; d++) {
                        unsigned int *dom = (unsigned int*)(buf + 8 + d * 32);
                        unsigned int present = dom[0];
                        unsigned int freq_khz = dom[1];
                        if (present & 1) {
                            printf("    Domain %d: %u MHz (present=0x%x)\n",
                                   d, freq_khz / 1000, present);
                        }
                    }
                }
            }
        }
    }

    dlclose(lib);
    return 0;
}
