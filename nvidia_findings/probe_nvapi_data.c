#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <stdint.h>

typedef void* (*QueryInterfaceFn)(uint32_t id);
typedef int32_t NvAPI_Status;
typedef void* NvPhysicalGpuHandle;

// NvAPI function types
typedef NvAPI_Status (*NvAPI_Initialize_t)(void);
typedef NvAPI_Status (*NvAPI_Unload_t)(void);
typedef NvAPI_Status (*NvAPI_EnumPhysicalGPUs_t)(NvPhysicalGpuHandle handles[64], uint32_t *count);
typedef NvAPI_Status (*NvAPI_GPU_GetFullName_t)(NvPhysicalGpuHandle handle, char name[64]);
typedef NvAPI_Status (*NvAPI_GPU_GetBusId_t)(NvPhysicalGpuHandle handle, uint32_t *busId);
typedef NvAPI_Status (*NvAPI_GetErrorMessage_t)(NvAPI_Status status, char msg[256]);

// Undocumented structs (large buffers to capture data)
typedef struct {
    uint32_t version;
    int32_t mask;
    int32_t values[40];
} NvApiThermals;

typedef struct {
    uint32_t version;
    uint32_t flags;
    uint32_t padding[6];
    uint32_t value_uv;
    uint32_t padding2[8];
} NvApiVoltage;

// Large generic struct for probing
typedef struct {
    uint32_t version;
    uint32_t data[512];
} GenericStruct;

typedef NvAPI_Status (*GenericGpuFn)(NvPhysicalGpuHandle handle, void *data);

QueryInterfaceFn QI;

void* get_fn(uint32_t id) { return QI(id); }

void dump_hex(const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    for (size_t i = 0; i < len; i++) {
        if (i % 32 == 0) printf("  %04zx: ", i);
        printf("%02x ", p[i]);
        if (i % 32 == 31) printf("\n");
    }
    if (len % 32 != 0) printf("\n");
}

int main() {
    void *lib = dlopen("libnvidia-api.so.1", RTLD_LAZY);
    if (!lib) { fprintf(stderr, "Cannot open libnvidia-api.so.1\n"); return 1; }
    QI = (QueryInterfaceFn)dlsym(lib, "nvapi_QueryInterface");
    if (!QI) { fprintf(stderr, "Cannot find nvapi_QueryInterface\n"); return 1; }

    NvAPI_Initialize_t init = (NvAPI_Initialize_t)get_fn(0x0150e828);
    NvAPI_EnumPhysicalGPUs_t enumGPUs = (NvAPI_EnumPhysicalGPUs_t)get_fn(0xe5ac921f);
    NvAPI_GPU_GetFullName_t getName = (NvAPI_GPU_GetFullName_t)get_fn(0xCEEE8E9F);
    NvAPI_GPU_GetBusId_t getBusId = (NvAPI_GPU_GetBusId_t)get_fn(0x1be0b8e5);
    NvAPI_GetErrorMessage_t getErr = (NvAPI_GetErrorMessage_t)get_fn(0x6c2d048c);

    NvAPI_Status st = init();
    printf("NvAPI_Initialize: %d\n", st);
    if (st != 0) return 1;

    NvPhysicalGpuHandle handles[64];
    uint32_t gpuCount = 0;
    st = enumGPUs(handles, &gpuCount);
    printf("EnumPhysicalGPUs: %d (count=%u)\n", st, gpuCount);

    for (uint32_t g = 0; g < gpuCount; g++) {
        char name[64] = {0};
        getName(handles[g], name);
        uint32_t busId = 0;
        getBusId(handles[g], &busId);
        printf("\n=== GPU %u: %s (bus %u) ===\n", g, name, busId);

        // 1. Thermal sensors (undocumented 0x65fe3aad)
        GenericGpuFn thermalFn = (GenericGpuFn)get_fn(0x65fe3aad);
        if (thermalFn) {
            NvApiThermals t;
            memset(&t, 0, sizeof(t));
            t.version = sizeof(NvApiThermals) | (2 << 16);  // version 2
            st = thermalFn(handles[g], &t);
            printf("\n[Thermal Sensors] status=%d\n", st);
            if (st == 0) {
                printf("  mask=0x%08x\n", t.mask);
                for (int i = 0; i < 40; i++) {
                    if (t.values[i] != 0) printf("  values[%d] = %d (temp=%d.%dC)\n", i, t.values[i], t.values[i]/256, (t.values[i]%256)*100/256);
                }
            } else {
                char msg[256]; getErr(st, msg); printf("  Error: %s\n", msg);
                // Try version 1
                memset(&t, 0, sizeof(t));
                t.version = sizeof(NvApiThermals) | (1 << 16);
                st = thermalFn(handles[g], &t);
                printf("  Retry v1: status=%d\n", st);
                if (st == 0) {
                    printf("  mask=0x%08x\n", t.mask);
                    for (int i = 0; i < 40; i++) {
                        if (t.values[i] != 0) printf("  values[%d] = %d (temp=%d.%dC)\n", i, t.values[i], t.values[i]/256, (t.values[i]%256)*100/256);
                    }
                }
            }
        }

        // 2. Voltage (undocumented 0x465f9bcf)
        GenericGpuFn voltageFn = (GenericGpuFn)get_fn(0x465f9bcf);
        if (voltageFn) {
            NvApiVoltage v;
            memset(&v, 0, sizeof(v));
            v.version = sizeof(NvApiVoltage) | (1 << 16);
            st = voltageFn(handles[g], &v);
            printf("\n[Voltage] status=%d\n", st);
            if (st == 0) {
                printf("  flags=0x%08x value_uv=%u (%.3f V)\n", v.flags, v.value_uv, v.value_uv/1000000.0);
                dump_hex(&v, sizeof(v));
            } else {
                char msg[256]; getErr(st, msg); printf("  Error: %s\n", msg);
            }
        }

        // 3. GetAllClockFrequencies (0x34C0B13D / 0xDCB616C3)
        GenericGpuFn clockFn = (GenericGpuFn)get_fn(0xDCB616C3);
        if (clockFn) {
            GenericStruct cs;
            memset(&cs, 0, sizeof(cs));
            cs.version = (sizeof(GenericStruct)) | (3 << 16);
            st = clockFn(handles[g], &cs);
            printf("\n[GetAllClockFrequencies_v2] status=%d\n", st);
            if (st == 0) {
                dump_hex(&cs, 256);
            } else {
                char msg[256]; getErr(st, msg); printf("  Error: %s\n", msg);
                // try v2
                memset(&cs, 0, sizeof(cs));
                cs.version = 288 | (2 << 16);
                st = clockFn(handles[g], &cs);
                printf("  Retry smaller: status=%d\n", st);
                if (st == 0) dump_hex(&cs, 288);
            }
        }

        // 4. GetPstates20_v2 (0x64B43A6A) 
        GenericGpuFn pstatesFn = (GenericGpuFn)get_fn(0x64B43A6A);
        if (pstatesFn) {
            GenericStruct ps;
            memset(&ps, 0, sizeof(ps));
            ps.version = sizeof(GenericStruct) | (2 << 16);
            st = pstatesFn(handles[g], &ps);
            printf("\n[GetPstates20_v2] status=%d\n", st);
            if (st == 0) {
                dump_hex(&ps, 256);
            } else {
                char msg[256]; getErr(st, msg); printf("  Error: %s\n", msg);
            }
        }

        // 5. ClientFanCoolersGetStatus (0x35AED5E8)
        GenericGpuFn fanStatusFn = (GenericGpuFn)get_fn(0x35AED5E8);
        if (fanStatusFn) {
            GenericStruct fs;
            memset(&fs, 0, sizeof(fs));
            fs.version = sizeof(GenericStruct) | (1 << 16);
            st = fanStatusFn(handles[g], &fs);
            printf("\n[ClientFanCoolersGetStatus] status=%d\n", st);
            if (st == 0) {
                dump_hex(&fs, 256);
            } else {
                char msg[256]; getErr(st, msg); printf("  Error: %s\n", msg);
            }
        }

        // 6. ClientFanCoolersGetControl (0x814B209F)
        GenericGpuFn fanCtrlFn = (GenericGpuFn)get_fn(0x814B209F);
        if (fanCtrlFn) {
            GenericStruct fc;
            memset(&fc, 0, sizeof(fc));
            fc.version = sizeof(GenericStruct) | (1 << 16);
            st = fanCtrlFn(handles[g], &fc);
            printf("\n[ClientFanCoolersGetControl] status=%d\n", st);
            if (st == 0) {
                dump_hex(&fc, 256);
            } else {
                char msg[256]; getErr(st, msg); printf("  Error: %s\n", msg);
            }
        }

        // 7. ClientClockBoostLockGetControl (0xE3640A56)
        GenericGpuFn boostLockFn = (GenericGpuFn)get_fn(0xE3640A56);
        if (boostLockFn) {
            GenericStruct bl;
            memset(&bl, 0, sizeof(bl));
            bl.version = sizeof(GenericStruct) | (1 << 16);
            st = boostLockFn(handles[g], &bl);
            printf("\n[ClientClockBoostLockGetControl] status=%d\n", st);
            if (st == 0) {
                dump_hex(&bl, 256);
            } else {
                char msg[256]; getErr(st, msg); printf("  Error: %s\n", msg);
            }
        }

        // 8. GetVoltageRange (0x891FA0AE)
        GenericGpuFn voltRangeFn = (GenericGpuFn)get_fn(0x891FA0AE);
        if (voltRangeFn) {
            GenericStruct vr;
            memset(&vr, 0, sizeof(vr));
            vr.version = sizeof(GenericStruct) | (1 << 16);
            st = voltRangeFn(handles[g], &vr);
            printf("\n[GetVoltageRange] status=%d\n", st);
            if (st == 0) {
                dump_hex(&vr, 128);
            } else {
                char msg[256]; getErr(st, msg); printf("  Error: %s\n", msg);
            }
        }

        // 9. ClientPowerTopologyGetInfo (0x60DED2ED)  
        GenericGpuFn pwrTopoInfo = (GenericGpuFn)get_fn(0x60DED2ED);
        if (pwrTopoInfo) {
            GenericStruct pt;
            memset(&pt, 0, sizeof(pt));
            pt.version = sizeof(GenericStruct) | (1 << 16);
            st = pwrTopoInfo(handles[g], &pt);
            printf("\n[ClientPowerTopologyGetInfo] status=%d\n", st);
            if (st == 0) {
                dump_hex(&pt, 256);
            } else {
                char msg[256]; getErr(st, msg); printf("  Error: %s\n", msg);
            }
        }

        // 10. ClientPowerTopologyGetStatus (0x6FF81213)
        GenericGpuFn pwrTopoStatus = (GenericGpuFn)get_fn(0x6FF81213);
        if (pwrTopoStatus) {
            GenericStruct ps;
            memset(&ps, 0, sizeof(ps));
            ps.version = sizeof(GenericStruct) | (1 << 16);
            st = pwrTopoStatus(handles[g], &ps);
            printf("\n[ClientPowerTopologyGetStatus] status=%d\n", st);
            if (st == 0) {
                dump_hex(&ps, 256);
            } else {
                char msg[256]; getErr(st, msg); printf("  Error: %s\n", msg);
            }
        }

        // 11. GetPerfClocks (0xEDCF624E)
        GenericGpuFn perfClockFn = (GenericGpuFn)get_fn(0xEDCF624E);
        if (perfClockFn) {
            GenericStruct pc;
            memset(&pc, 0, sizeof(pc));
            pc.version = sizeof(GenericStruct) | (1 << 16);
            st = perfClockFn(handles[g], &pc);
            printf("\n[GetPerfClocks] status=%d\n", st);
            if (st == 0) {
                dump_hex(&pc, 256);
            } else {
                char msg[256]; getErr(st, msg); printf("  Error: %s\n", msg);
            }
        }

        // 12. PerfPoliciesGetStatus (0x3D358A0C)
        GenericGpuFn perfPolStatus = (GenericGpuFn)get_fn(0x3D358A0C);
        if (perfPolStatus) {
            GenericStruct pp;
            memset(&pp, 0, sizeof(pp));
            pp.version = sizeof(GenericStruct) | (1 << 16);
            st = perfPolStatus(handles[g], &pp);
            printf("\n[PerfPoliciesGetStatus] status=%d\n", st);
            if (st == 0) {
                dump_hex(&pp, 256);
            } else {
                char msg[256]; getErr(st, msg); printf("  Error: %s\n", msg);
            }
        }

        // 13. SetPstates20 (0x0F4DAE6B) - just check it resolves, don't call
        void *setPstateFn = get_fn(0x0F4DAE6B);
        printf("\n[SetPstates20] available: %s\n", setPstateFn ? "YES" : "no");

        // 14. ClientPowerPoliciesGetStatus (0x70916171)
        GenericGpuFn pwrPolStatus = (GenericGpuFn)get_fn(0x70916171);
        if (pwrPolStatus) {
            GenericStruct pp;
            memset(&pp, 0, sizeof(pp));
            pp.version = sizeof(GenericStruct) | (1 << 16);
            st = pwrPolStatus(handles[g], &pp);
            printf("\n[ClientPowerPoliciesGetStatus] status=%d\n", st);
            if (st == 0) {
                dump_hex(&pp, 256);
            } else {
                char msg[256]; getErr(st, msg); printf("  Error: %s\n", msg);
            }
        }
    }

    NvAPI_Unload_t unload = (NvAPI_Unload_t)get_fn(0xd22bdd7e);
    unload();
    dlclose(lib);
    return 0;
}
