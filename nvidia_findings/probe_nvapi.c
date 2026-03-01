#include <stdio.h>
#include <dlfcn.h>

typedef void* (*QueryInterfaceFn)(unsigned int id);

// Known NvAPI function IDs (many from Windows reverse-engineering)
struct { unsigned int id; const char *name; } nvapi_ids[] = {
    { 0x0150e828, "NvAPI_Initialize" },
    { 0xd22bdd7e, "NvAPI_Unload" },
    { 0xe5ac921f, "NvAPI_EnumPhysicalGPUs" },
    { 0x1be0b8e5, "NvAPI_GPU_GetBusId" },
    { 0x6c2d048c, "NvAPI_GetErrorMessage" },
    { 0x65fe3aad, "NvAPI_GPU_ThermalGetSensors (undocumented)" },
    { 0x465f9bcf, "NvAPI_GPU_GetCurrentVoltage (undocumented)" },
    // Clocks and OC
    { 0x0733D01C, "NvAPI_GPU_GetPstates20" },
    { 0x0F4DAE6B, "NvAPI_GPU_SetPstates20" },
    { 0x6FF81213, "NvAPI_GPU_ClientPowerTopologyGetStatus (undoc)" },
    { 0x34C0B13D, "NvAPI_GPU_GetAllClockFrequencies" },
    { 0xDCB616C3, "NvAPI_GPU_GetAllClockFrequencies_v2" },
    { 0xB7B36837, "NvAPI_GPU_GetCurrentBoostState" },
    { 0x137F0DF1, "NvAPI_GPU_GetVoltageDomainsStatus" },
    { 0x0EDCF624E, "NvAPI_GPU_GetPerfClocks" },
    { 0x1EA54A3B, "NvAPI_GPU_GetPerfClocks_v2" },
    { 0x6A163D2D, "NvAPI_GPU_GetDynamicPstatesInfoEx" },
    // V/F Curve related
    { 0x28C7D86, "NvAPI_GPU_GetVFPointsInfo" },
    { 0xCC76A3, "NvAPI_GPU_RestoreVFPoints" },
    { 0x34A50B4, "NvAPI_GPU_LockVFPoints" },
    { 0x891FA0AE, "NvAPI_GPU_GetVoltageRange" },
    // Fan control
    { 0xDA141340, "NvAPI_GPU_GetCoolerSettings" },
    { 0x891FA0AE, "NvAPI_GPU_SetCoolerLevels" },
    { 0xFB85B01C, "NvAPI_GPU_GetCoolerPolicyTable" },
    { 0x987947CD, "NvAPI_GPU_SetCoolerPolicyTable" },
    { 0x35AED5E8, "NvAPI_GPU_ClientFanCoolersGetStatus" },
    { 0x814B209F, "NvAPI_GPU_ClientFanCoolersGetControl" },
    { 0xA58971A5, "NvAPI_GPU_ClientFanCoolersSetControl" },
    // Power
    { 0xD81E7D7, "NvAPI_GPU_PowerMonitor_GetPowerUsage" },
    { 0xBE52A372, "NvAPI_GPU_GetPowerUsage" },
    { 0xABC54E75, "NvAPI_GPU_GetThermalTable" },
    // Boost/OC
    { 0xE3640A56, "NvAPI_GPU_ClientClockBoostLockGetControl" },
    { 0x2CFDCEC, "NvAPI_GPU_GetOverclockingControl" },
    { 0x4309E690, "NvAPI_GPU_SetOverclockingProfile" },
    { 0xF4CEF23, "NvAPI_GPU_GetBaseVoltage" },
    // Clock ranges
    { 0x64B43A6A, "NvAPI_GPU_GetPstates20_v2" },
    { 0x60DED2ED, "NvAPI_GPU_ClientPowerTopologyGetInfo" },
    { 0x0296D378C, "NvAPI_GPU_ClientPowerPoliciesGetInfo" },
    { 0x70916171, "NvAPI_GPU_ClientPowerPoliciesGetStatus" },
    { 0x0AD95F5ED, "NvAPI_GPU_ClientPowerPoliciesSetStatus" },
    // Performance
    { 0x6B7B529, "NvAPI_GPU_PerfPoliciesGetInfo" },
    { 0x3D358A0C, "NvAPI_GPU_PerfPoliciesGetStatus" },
    // Misc
    { 0xE3795199, "NvAPI_GPU_GetShortName" },
    { 0xCEEE8E9F, "NvAPI_GPU_GetFullName" },
    { 0x49DCECD, "NvAPI_GPU_GetPhysicalFrameBufferSize" },
    { 0x5A04B644, "NvAPI_GPU_GetVirtualFrameBufferSize" },
    { 0xC33BAEB1, "NvAPI_GPU_GetBoardInfo" },
    { 0x774AA982, "NvAPI_GPU_GetMemoryInfo" },
    { 0xE24CEEE, "NvAPI_GPU_GetMemoryInfo_v3" },
    { 0, NULL }
};

int main() {
    void *lib = dlopen("libnvidia-api.so.1", RTLD_LAZY);
    if (!lib) { fprintf(stderr, "Cannot open libnvidia-api.so.1: %s\n", dlerror()); return 1; }
    
    QueryInterfaceFn queryInterface = (QueryInterfaceFn)dlsym(lib, "nvapi_QueryInterface");
    if (!queryInterface) { fprintf(stderr, "Cannot find nvapi_QueryInterface\n"); return 1; }
    
    printf("%-12s %-50s %s\n", "ID", "Function", "Available");
    printf("%-12s %-50s %s\n", "----", "--------", "---------");
    
    for (int i = 0; nvapi_ids[i].name; i++) {
        void *fn = queryInterface(nvapi_ids[i].id);
        printf("0x%08X  %-50s %s\n", nvapi_ids[i].id, nvapi_ids[i].name, fn ? "YES" : "no");
    }
    
    dlclose(lib);
    return 0;
}
