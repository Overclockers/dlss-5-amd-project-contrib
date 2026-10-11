#include <windows.h>
#include <cstdio>
#include <stdexcept>
#include <string>
static unsigned requested = 0;
static HMODULE ProductLoad(unsigned version) { requested = version; return GetModuleHandleW(L"kernel32.dll"); }
#define HIP_PROBE_RUNTIME_MODULE(version) ProductLoad(version)
#include "third_party/lmxxf/Development/HIP/hip_api.h"
#undef HIP_PROBE_RUNTIME_MODULE
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/lmxxf_runtime/LmxxfHipRuntimeLoad.h"
int main() {
    try { hip_probe::Api api; return 1; }
    catch (const std::runtime_error& e) {
        if (requested != 7 || std::string(e.what()).find("missing HIP export") == std::string::npos) return 2;
    }
    try { LmxxfHipRuntimeLoad::Load(6); return 3; }
    catch (const std::runtime_error& e) { if (std::string(e.what()).find("requires HIP 7") == std::string::npos) return 4; }
    std::puts("HIP loader injection preserves export validation and rejects HIP 6 PASS");
}
