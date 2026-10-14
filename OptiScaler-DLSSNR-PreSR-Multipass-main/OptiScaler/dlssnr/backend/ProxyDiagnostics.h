#pragma once
// Diagnostics for the "missing recording proxy" path shared by the lmxxf and
// Mochizuki backends.
//
// Both backends call QueryInterface(ILogicalCommandList) on the game's command
// list. About half of the lists in Forza Horizon 6 fail that QI (E_NOINTERFACE):
// they were never wrapped by SubmissionHooks, so the frame falls back to the
// original colour and NR and the original alternate. The root cause is not yet
// pinned down, so this header records *where a failing list came from*:
//
//   - the list's vtable pointer and the module that owns it. The vtable usually
//     lives in the module that implemented the object (d3d12core.dll for a real
//     list, or the game exe for its own wrapper),
//   - the two closest module bases around the address when no module claims it,
//     so a pointer into freed heap or a driver mapping still says something,
//   - the device the caller recorded a successful list on last, so two-device
//     setups (e.g. an iGPU present alongside the dGPU) can be told apart from a
//     single device whose lists are partly unwrapped.
//
// This adds no behaviour: it only enriches the existing throttled warning.

#include <Windows.h>
#include <d3d12.h>
#include <cstdint>
#include <cstdio>

namespace DlssNr::Backend::ProxyDiagnostics
{
// Copy the file name (no path) of `path` into `out`.
inline void BaseName(const char* path, char* out, size_t outBytes)
{
    if (!out || outBytes == 0) return;
    const char* base = path;
    for (const char* p = path; *p; ++p)
        if (*p == '\\' || *p == '/') base = p + 1;
    std::snprintf(out, outBytes, "%s", base);
}

// Direct PEB walk: some mappings (a module loaded after the sample, or one this
// process cannot query) do not answer GetModuleHandleExW(FROM_ADDRESS). The PEB
// loader list is the same data the loader itself uses, so it sees every module
// whose headers are still mapped. Returns the module base or nullptr.
inline HMODULE ModuleFromPeb(const void* address, char* nameOut, size_t nameBytes,
                             unsigned long long* baseOut)
{
#if defined(_M_X64) || defined(_M_AMD64)
    const auto addr = reinterpret_cast<unsigned long long>(address);
    const auto* peb = reinterpret_cast<const unsigned char*>(__readgsqword(0x60));
    if (!peb) return nullptr;
    const auto* ldr = *reinterpret_cast<const unsigned char* const*>(peb + 0x18);
    if (!ldr) return nullptr;
    const auto* head = ldr + 0x10; // InLoadOrderModuleList
    const auto* node = *reinterpret_cast<const unsigned char* const*>(head);
    const auto* tail = *reinterpret_cast<const unsigned char* const*>(reinterpret_cast<const unsigned char*>(head) + 8);
    // Walk the doubly linked list; LDR_DATA_TABLE_ENTRY.DllBase is at +0x30 and
    // its BaseDllName UNICODE_STRING at +0x58 (x64, both Win10 and Win11).
    for (int guard = 0; node && node != head && guard < 4096; ++guard)
    {
        const auto base = *reinterpret_cast<const unsigned long long*>(node + 0x30);
        const auto size = *reinterpret_cast<const unsigned int*>(node + 0x40);
        if (base && addr >= base && addr < base + (size ? size : 0x10000000ull))
        {
            const auto* ustr = node + 0x58; // FullDllName buffer
            const auto len = *reinterpret_cast<const unsigned short*>(ustr) / sizeof(wchar_t);
            const auto buf = *reinterpret_cast<const wchar_t* const*>(ustr + 8);
            *baseOut = base;
            if (nameOut && nameBytes && buf && len)
            {
                char narrow[MAX_PATH] {};
                const int n = WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(len),
                                                  narrow, MAX_PATH - 1, nullptr, nullptr);
                if (n > 0) { narrow[n] = '\0'; BaseName(narrow, nameOut, nameBytes); }
            }
            return reinterpret_cast<HMODULE>(base);
        }
        node = *reinterpret_cast<const unsigned char* const*>(node);
        if (!node || node == tail) break;
    }
#else
    (void)address; (void)nameOut; (void)nameBytes; (void)baseOut;
#endif
    return nullptr;
}

// Module base name (without path) that owns `address`, or "?" when unknown.
inline void ModuleNameFor(const void* address, char* out, size_t outBytes)
{
    if (!out || outBytes == 0) return;
    out[0] = '?'; out[1] = '\0';
    if (!address) return;
    HMODULE owner = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &owner) && owner)
    {
        char path[MAX_PATH] {};
        const DWORD n = GetModuleFileNameA(owner, path, MAX_PATH);
        if (n != 0) { BaseName(path, out, outBytes); return; }
    }
    unsigned long long base = 0;
    if (ModuleFromPeb(address, out, outBytes, &base))
        return;
    out[0] = '?'; out[1] = '\0';
}

// The vtable pointer is the object's first machine word. Reading it is safe for
// any COM object that reached this code path (the QI already dereferenced it).
inline const void* VTableOf(const void* object) noexcept
{
    return object ? *reinterpret_cast<const void* const*>(object) : nullptr;
}

// Writes "vtable=0x… owner=module.dll" into the caller's buffer.
inline void DescribeList(const void* object, char* out, size_t outBytes)
{
    if (!out || outBytes == 0) return;
    const void* vtable = VTableOf(object);
    char module[MAX_PATH] {};
    ModuleNameFor(vtable, module, sizeof module);
    std::snprintf(out, outBytes, "vtable=%p owner=%s", vtable, module);
}
}
