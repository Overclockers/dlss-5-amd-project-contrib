#pragma once
#include "../../amd/HipRuntimeLoad.h"
#include <mutex>

namespace LmxxfHipRuntimeLoad
{
// Independent of the game's current directory; no global DLL-search mutations.
inline void Log(const std::string& message) noexcept
{
    try {
        OutputDebugStringA(("lmxxf: " + message + "\n").c_str());
        HMODULE self = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&Log), &self)) return;
        AmdPreSr::HipRuntimeLoad::WindowsApi api;
        auto path = std::filesystem::path(api.ModulePath(self)).parent_path() / L"lmxxf_runtime.log";
        HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        const auto line = message + "\r\n";
        DWORD written = 0; WriteFile(file, line.data(), DWORD(line.size()), &written, nullptr); CloseHandle(file);
    } catch (...) {} // Diagnostics cannot turn a valid load into a fatal error.
}
inline HMODULE Load(unsigned version)
{
    if (version != 7) throw std::runtime_error("lmxxf requires HIP 7; update the complete package and compatible HIP runtime");
    static std::mutex mutex;
    static HMODULE selected = nullptr;
    std::lock_guard lock(mutex);
    if (selected) return selected;
    AmdPreSr::HipRuntimeLoad::WindowsApi api;
    // This is the same controlled HIP 7 search used by our Daniel adapter: default directories,
    // absolute HIP_PATH/bin, then absolute PATH entries with dependency-aware flags.
    selected = AmdPreSr::HipRuntimeLoad::SelectModule(api, [](const std::string& text) { Log(text); });
    const auto query = reinterpret_cast<int (*)(int*)>(GetProcAddress(selected, "hipRuntimeGetVersion"));
    int runtime = 0;
    const int rc = query ? query(&runtime) : -1;
    Log("HIP runtime version=" + std::to_string(runtime) + " queryResult=" + std::to_string(rc));
    return selected; // Retain for process lifetime, including later export/init failures.
}
}
