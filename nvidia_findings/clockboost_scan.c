/*
 * clockboost_scan.c - Brute-force scan for NvAPI ClockBoostLock/Table/VFPCurve
 * Tries sizes up to 32768 and versions 1-5
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <signal.h>
#include <setjmp.h>

typedef void* NvPhysicalGpuHandle;
typedef int NvAPI_Status;
typedef void* (*QueryInterfaceFn)(unsigned int);

static jmp_buf jump_buf;
static volatile sig_atomic_t got_signal = 0;

static void signal_handler(int sig) {
    got_signal = 1;
    longjmp(jump_buf, 1);
}

/* NvAPI function IDs */
#define NVAPI_INITIALIZE       0x0150E828
#define NVAPI_ENUM_GPUS        0xE5AC921F
#define NVAPI_GET_ERROR_MSG    0x6C2D048C

/* Target functions */
struct target_func {
    const char *name;
    unsigned int id;
};

static struct target_func targets[] = {
    { "GetClockBoostLock",       0xE440B867 },
    { "GetClockBoostTable",      0x23F1B133 },
    { "GetVFPCurve",             0x21537AD4 },
    { "GetClockBoostRanges",     0x64B43A6A },
    { "GetPstates20",            0x6FF81213 },
    { "GetCurrentPstate",        0x927DA4F6 },
    { "GetPstatesInfoEx",        0x843C0256 },
    { "GetClockFrequencies",     0xDCB616C3 },
    { "GetDynamicPstatesInfoEx", 0x60DED2ED }, /* ClientPowerTopology - recheck */
    { "Unknown_0x0D258BB5",      0x0D258BB5 },
};
#define NUM_TARGETS (sizeof(targets)/sizeof(targets[0]))

int main(void) {
    void *lib = dlopen("libnvidia-api.so.1", RTLD_NOW);
    if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }

    QueryInterfaceFn queryInterface = dlsym(lib, "nvapi_QueryInterface");
    if (!queryInterface) { fprintf(stderr, "no queryInterface\n"); return 1; }

    /* Initialize */
    typedef NvAPI_Status (*InitFn)(void);
    InitFn init = (InitFn)queryInterface(NVAPI_INITIALIZE);
    if (!init || init() != 0) { fprintf(stderr, "init failed\n"); return 1; }

    /* Enum GPUs */
    typedef NvAPI_Status (*EnumGpusFn)(NvPhysicalGpuHandle*, unsigned int*);
    EnumGpusFn enumGpus = (EnumGpusFn)queryInterface(NVAPI_ENUM_GPUS);
    NvPhysicalGpuHandle handles[64];
    unsigned int count = 0;
    if (!enumGpus || enumGpus(handles, &count) != 0 || count == 0) {
        fprintf(stderr, "no GPUs\n"); return 1;
    }
    NvPhysicalGpuHandle gpu = handles[0];

    /* Error message */
    typedef NvAPI_Status (*ErrMsgFn)(NvAPI_Status, char*);
    ErrMsgFn getErrMsg = (ErrMsgFn)queryInterface(NVAPI_GET_ERROR_MSG);

    printf("=== ClockBoost/VFPCurve Extended Scan ===\n");
    printf("GPU handle: %p, scanning %zu targets\n\n", gpu, NUM_TARGETS);

    /* Install signal handler */
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);

    for (int t = 0; t < NUM_TARGETS; t++) {
        void *fn_ptr = queryInterface(targets[t].id);
        if (!fn_ptr) {
            printf("[%s] 0x%08X: NOT AVAILABLE\n\n", targets[t].name, targets[t].id);
            continue;
        }
        printf("[%s] 0x%08X: available at %p\n", targets[t].name, targets[t].id, fn_ptr);

        typedef NvAPI_Status (*NvGpuFn)(NvPhysicalGpuHandle, void*);
        NvGpuFn func = (NvGpuFn)fn_ptr;

        int found = 0;

        /* Scan sizes: key sizes that might work */
        int sizes[] = {
            8, 12, 16, 20, 24, 28, 32, 36, 40, 48, 56, 64, 72, 80, 88, 96,
            104, 112, 120, 128, 136, 144, 152, 160, 168, 176, 184, 192, 200,
            208, 216, 224, 232, 240, 248, 256, 264, 272, 280, 288, 296,
            304, 312, 320, 328, 336, 344, 352, 360, 368, 376, 384, 392, 400,
            408, 416, 424, 432, 440, 448, 456, 464, 472, 480, 488, 496, 504, 512,
            520, 528, 536, 544, 552, 560, 568, 576, 584, 592, 600, 608, 616, 624, 632, 640,
            648, 656, 664, 672, 680, 688, 696, 704, 712, 720, 728, 736, 744, 752, 760, 768,
            800, 832, 864, 896, 928, 960, 992, 1024,
            1088, 1152, 1216, 1280, 1344, 1408, 1472, 1536,
            1600, 1664, 1728, 1792, 1856, 1920, 1984, 2048,
            2176, 2304, 2432, 2560, 2688, 2816, 2944, 3072,
            3200, 3328, 3456, 3584, 3712, 3840, 3968, 4096,
            4352, 4608, 4864, 5120, 5376, 5632, 5888, 6144,
            6400, 6656, 6912, 7168, 7424, 7680, 7936, 8192,
            8704, 9216, 9728, 10240, 10752, 11264, 11776, 12288,
            12800, 13312, 13824, 14336, 14848, 15360, 15872, 16384,
            17408, 18432, 19456, 20480, 24576, 28672, 32768,
            0 /* sentinel */
        };

        for (int si = 0; sizes[si] != 0; si++) {
            int sz = sizes[si];
            for (int ver = 1; ver <= 5; ver++) {
                unsigned char *buf = calloc(1, sz + 4096); /* extra safety */
                if (!buf) continue;

                unsigned int version_word = (sz & 0xFFFF) | (ver << 16);
                *(unsigned int*)buf = version_word;

                got_signal = 0;
                NvAPI_Status status;

                if (setjmp(jump_buf) == 0) {
                    status = func(gpu, buf);
                } else {
                    status = -999; /* signal caught */
                }

                if (status == 0) {
                    /* SUCCESS! Check if buffer has non-zero data */
                    int has_data = 0;
                    for (int i = 4; i < sz; i++) {
                        if (buf[i] != 0) { has_data = 1; break; }
                    }
                    printf("  *** SUCCESS: sz=%d ver=%d (0x%08X) has_data=%d\n",
                           sz, ver, version_word, has_data);

                    if (has_data) {
                        printf("    First 64 bytes (hex): ");
                        int dump_len = sz < 64 ? sz : 64;
                        for (int i = 0; i < dump_len; i++) printf("%02x", buf[i]);
                        printf("\n");

                        /* Also dump as u32 array */
                        printf("    As u32[]: ");
                        int num_u32 = (dump_len + 3) / 4;
                        unsigned int *u32_buf = (unsigned int*)buf;
                        for (int i = 0; i < num_u32; i++) printf("%u ", u32_buf[i]);
                        printf("\n");
                    }
                    found = 1;
                } else if (status == -999) {
                    printf("  SIGNAL at sz=%d ver=%d - skipping larger\n", sz, ver);
                    free(buf);
                    goto next_target;
                }
                /* Silently skip version mismatch (-36), invalid argument (-5), etc. */

                free(buf);
            }
        }
next_target:
        if (!found) {
            printf("  No working version/size found\n");
        }
        printf("\n");
    }

    printf("=== Scan Complete ===\n");
    dlclose(lib);
    return 0;
}
