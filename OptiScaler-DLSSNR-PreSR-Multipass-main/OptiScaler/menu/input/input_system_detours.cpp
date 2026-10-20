#include "pch.h"
#include "input_system_internal.h"

#include <detours/detours.h>

#include <tlhelp32.h>
#include <vector>
#include <string_view>

static bool messageHooks = false;
static bool keyStateHooks = false;
static bool getPosHooks = false;
static bool clipCursorHooks = false;
static bool message2Hooks = false;
static bool hidHooks = false;
static bool rawHooks = false;
static bool windowsHooks = false;
static bool positionHooks = false;
static bool positionIATHooks = false;

namespace OptiInput
{
SHORT RealGetAsyncKeyStateSafe(int vk)
{
    ScopedHookBypass bypass;

    if (o_GetAsyncKeyState != nullptr)
        return o_GetAsyncKeyState(vk);

    return ::GetAsyncKeyState(vk);
}

SHORT RealGetKeyStateSafe(int vk)
{
    ScopedHookBypass bypass;

    if (o_GetKeyState != nullptr)
        return o_GetKeyState(vk);

    return ::GetKeyState(vk);
}

BOOL RealGetCursorPosSafe(LPPOINT point)
{
    ScopedHookBypass bypass;

    if (o_GetCursorPos != nullptr)
        return o_GetCursorPos(point);

    return ::GetCursorPos(point);
}

namespace
{
struct IatPatch
{
    void** Entry = nullptr;
    void* Original = nullptr;
    void* Detour = nullptr;
    std::string Name;
    HMODULE Owner = nullptr;
};

std::vector<IatPatch> gCursorIatPatches;
std::vector<HMODULE> restoredCursorIatOwners;

bool IsSelfModule(HMODULE module)
{
    HMODULE selfModule = nullptr;

    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&InstallHooks), &selfModule))
    {
        return false;
    }

    return module == selfModule;
}

bool PatchIatEntry(HMODULE module, const char* importName, void* original, void* detour)
{
    if (module == nullptr || importName == nullptr || original == nullptr || detour == nullptr)
        return false;

    if (IsSelfModule(module))
        return false;

    auto* base = reinterpret_cast<std::uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);

    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);

    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return false;

    const auto& importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];

    if (importDir.VirtualAddress == 0 || importDir.Size == 0)
        return false;

    auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + importDir.VirtualAddress);
    bool patched = false;

    for (; desc->Name != 0; ++desc)
    {
        auto* thunkOrig = desc->OriginalFirstThunk != 0
                              ? reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk)
                              : nullptr;

        auto* thunkIat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);

        if (thunkIat == nullptr)
            continue;

        for (size_t index = 0; thunkIat[index].u1.Function != 0; ++index)
        {
            const char* name = nullptr;

            if (thunkOrig != nullptr && !(thunkOrig[index].u1.Ordinal & IMAGE_ORDINAL_FLAG))
            {
                auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + thunkOrig[index].u1.AddressOfData);
                name = reinterpret_cast<const char*>(byName->Name);
            }

            if (name == nullptr || strcmp(name, importName) != 0)
                continue;

            auto** entry = reinterpret_cast<void**>(&thunkIat[index].u1.Function);

            // Only patch entries that still point to the real API.
            // This avoids overwriting another hook chain.
            if (*entry != original)
            {
                LOG_DEBUG("OptiInput IAT skip {} module:{} entry:{} current:{} expected:{}", importName,
                          static_cast<void*>(module), static_cast<void*>(entry), *entry, original);
                continue;
            }

            DWORD oldProtect = 0;
            if (!VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &oldProtect))
            {
                LOG_WARN("OptiInput IAT protect failed {} module:{} entry:{} gle:{}", importName,
                         static_cast<void*>(module), static_cast<void*>(entry), GetLastError());
                continue;
            }

            HMODULE owner = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                   reinterpret_cast<LPCWSTR>(entry), &owner))
            {
                DWORD ignored = 0;
                VirtualProtect(entry, sizeof(void*), oldProtect, &ignored);
                continue;
            }
            *entry = detour;

            DWORD ignored = 0;
            VirtualProtect(entry, sizeof(void*), oldProtect, &ignored);
            FlushInstructionCache(GetCurrentProcess(), entry, sizeof(void*));

            gCursorIatPatches.push_back({ entry, original, detour, importName, owner });

            LOG_INFO("OptiInput IAT patched {} module:{} entry:{} original:{} detour:{}", importName,
                     static_cast<void*>(module), static_cast<void*>(entry), original, detour);

            patched = true;
        }
    }

    return patched;
}

bool PatchCursorIatForModule(HMODULE module)
{
    bool patched = false;

    patched |= PatchIatEntry(module, "GetCursorPos", reinterpret_cast<void*>(o_GetCursorPos),
                             reinterpret_cast<void*>(hkGetCursorPos));

    patched |= PatchIatEntry(module, "SetCursorPos", reinterpret_cast<void*>(o_SetCursorPos),
                             reinterpret_cast<void*>(hkSetCursorPos));

    return patched;
}

bool InstallCursorIatHooks()
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        LOG_WARN("OptiInput cursor IAT snapshot failed gle:{}", GetLastError());
        return false;
    }

    MODULEENTRY32W entry {};
    entry.dwSize = sizeof(entry);

    bool patchedAny = false;

    if (Module32FirstW(snapshot, &entry))
    {
        do
        {
            patchedAny |= PatchCursorIatForModule(entry.hModule);
        } while (Module32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);

    LOG_INFO("OptiInput cursor IAT hooks installed patched:{} entries:{}", patchedAny ? 1 : 0,
             gCursorIatPatches.size());

    return patchedAny;
}

bool RemoveCursorIatHooks()
{
    for (auto it = gCursorIatPatches.begin(); it != gCursorIatPatches.end();)
    {
        IatPatch& patch = *it;
        // A different hook may have captured our IAT callback. Do not lose its
        // lifetime record or overwrite a chain that we no longer own.
        if (*patch.Entry != patch.Detour && *patch.Entry != patch.Original)
        {
            ++it;
            continue;
        }
        if (*patch.Entry == patch.Detour)
        {
            DWORD oldProtect = 0;
            if (!VirtualProtect(patch.Entry, sizeof(void*), PAGE_READWRITE, &oldProtect))
            {
                ++it;
                continue;
            }
            InterlockedCompareExchangePointer(patch.Entry, patch.Original, patch.Detour);
            DWORD ignored = 0;
            VirtualProtect(patch.Entry, sizeof(void*), oldProtect, &ignored);
            FlushInstructionCache(GetCurrentProcess(), patch.Entry, sizeof(void*));
            if (*patch.Entry != patch.Original)
            {
                ++it;
                continue;
            }
        }
        restoredCursorIatOwners.push_back(patch.Owner);
        it = gCursorIatPatches.erase(it);
    }
    return gCursorIatPatches.empty();
}


void ResolveOptionalUser32Exports()
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");

    if (user32 == nullptr)
        user32 = LoadLibraryW(L"user32.dll");

    if (user32 == nullptr)
        return;

    if (o_GetPhysicalCursorPos == nullptr)
    {
        o_GetPhysicalCursorPos =
            reinterpret_cast<GetPhysicalCursorPos_t>(GetProcAddress(user32, "GetPhysicalCursorPos"));
    }

    if (o_SetPhysicalCursorPos == nullptr)
    {
        o_SetPhysicalCursorPos =
            reinterpret_cast<SetPhysicalCursorPos_t>(GetProcAddress(user32, "SetPhysicalCursorPos"));
    }
}

} // namespace
std::vector<HMODULE> TakeRestoredCursorIatOwnersLocked()
{
    std::vector<HMODULE> owners;
    owners.swap(restoredCursorIatOwners);
    return owners;
}

bool InstallHooks()
{
    if (_state.HooksInstalled)
        return true;
    if (!RetainInputCallbackModule(reinterpret_cast<const void*>(&InstallHooks)))
        return false;

    LOG_INFO("installing Win32 input hooks currentPid:{} targetPid:{} inputPid:{}", _state.CurrentProcessId,
             _state.TargetProcessId, _state.InputProcessId);

    ResolveOptionalUser32Exports();

    if (!messageHooks)
    {
        HookLifetime::Transaction<DetourBackend> transaction;

        transaction.Attach(reinterpret_cast<void**>(&o_PeekMessageA), reinterpret_cast<void*>(hkPeekMessageA));
        transaction.Attach(reinterpret_cast<void**>(&o_PeekMessageW), reinterpret_cast<void*>(hkPeekMessageW));
        transaction.Attach(reinterpret_cast<void**>(&o_GetMessageA), reinterpret_cast<void*>(hkGetMessageA));
        transaction.Attach(reinterpret_cast<void**>(&o_GetMessageW), reinterpret_cast<void*>(hkGetMessageW));

        const LONG result = transaction.Commit() ? NO_ERROR : ERROR_INVALID_FUNCTION;

        messageHooks = result == NO_ERROR;
        if (messageHooks)
            LOG_INFO("Win32 message hooks installed");
        else
            LOG_ERROR("Win32 message hook installation failed result:{}", result);
    }

    if (!keyStateHooks)
    {
        HookLifetime::Transaction<DetourBackend> transaction;

        transaction.Attach(reinterpret_cast<void**>(&o_GetAsyncKeyState), reinterpret_cast<void*>(hkGetAsyncKeyState));
        transaction.Attach(reinterpret_cast<void**>(&o_GetKeyState), reinterpret_cast<void*>(hkGetKeyState));
        transaction.Attach(reinterpret_cast<void**>(&o_GetKeyboardState), reinterpret_cast<void*>(hkGetKeyboardState));

        const LONG result = transaction.Commit() ? NO_ERROR : ERROR_INVALID_FUNCTION;

        keyStateHooks = result == NO_ERROR;
        if (keyStateHooks)
            LOG_INFO("Win32 key state hooks installed");
        else
            LOG_ERROR("Win32 key state hook installation failed result:{}", result);
    }

    if (!getPosHooks)
    {
        HookLifetime::Transaction<DetourBackend> transaction;

        transaction.Attach(reinterpret_cast<void**>(&o_GetMessagePos), reinterpret_cast<void*>(hkGetMessagePos));
        transaction.Attach(reinterpret_cast<void**>(&o_GetMouseMovePointsEx), reinterpret_cast<void*>(hkGetMouseMovePointsEx));

        const LONG result = transaction.Commit() ? NO_ERROR : ERROR_INVALID_FUNCTION;

        getPosHooks = result == NO_ERROR;
        if (getPosHooks)
            LOG_INFO("Win32 GetMessagePos hooks installed");
        else
            LOG_ERROR("Win32 GetMessagePos hook installation failed result:{}", result);
    }

    if (!clipCursorHooks)
    {
        HookLifetime::Transaction<DetourBackend> transaction;

        transaction.Attach(reinterpret_cast<void**>(&o_ClipCursor), reinterpret_cast<void*>(hkClipCursor));
        transaction.Attach(reinterpret_cast<void**>(&o_GetClipCursor), reinterpret_cast<void*>(hkGetClipCursor));

        const LONG result = transaction.Commit() ? NO_ERROR : ERROR_INVALID_FUNCTION;

        clipCursorHooks = result == NO_ERROR;
        if (clipCursorHooks)
            LOG_INFO("Win32 clip cursor hooks installed");
        else
            LOG_ERROR("Win32 clip cursor hook installation failed result:{}", result);
    }

    if (!message2Hooks)
    {
        HookLifetime::Transaction<DetourBackend> transaction;

        transaction.Attach(reinterpret_cast<void**>(&o_SendInput), reinterpret_cast<void*>(hkSendInput));
        transaction.Attach(reinterpret_cast<void**>(&o_mouse_event), reinterpret_cast<void*>(hkmouse_event));
        transaction.Attach(reinterpret_cast<void**>(&o_PostMessageA), reinterpret_cast<void*>(hkPostMessageA));
        transaction.Attach(reinterpret_cast<void**>(&o_PostMessageW), reinterpret_cast<void*>(hkPostMessageW));
        transaction.Attach(reinterpret_cast<void**>(&o_SendMessageA), reinterpret_cast<void*>(hkSendMessageA));
        transaction.Attach(reinterpret_cast<void**>(&o_SendMessageW), reinterpret_cast<void*>(hkSendMessageW));

        const LONG result = transaction.Commit() ? NO_ERROR : ERROR_INVALID_FUNCTION;

        message2Hooks = result == NO_ERROR;
        if (message2Hooks)
            LOG_INFO("Win32 message hooks installed");
        else
            LOG_ERROR("Win32 message hook installation failed result:{}", result);
    }

    if (!State::Instance().isRunningOnLinux && !hidHooks)
    {
        HookLifetime::Transaction<DetourBackend> transaction;

        transaction.Attach(reinterpret_cast<void**>(&o_CreateFileA), reinterpret_cast<void*>(hkCreateFileA));
        transaction.Attach(reinterpret_cast<void**>(&o_CreateFileW), reinterpret_cast<void*>(hkCreateFileW));
        transaction.Attach(reinterpret_cast<void**>(&o_ReadFile), reinterpret_cast<void*>(hkReadFile));
        transaction.Attach(reinterpret_cast<void**>(&o_DeviceIoControl), reinterpret_cast<void*>(hkDeviceIoControl));
        transaction.Attach(reinterpret_cast<void**>(&o_CloseHandle), reinterpret_cast<void*>(hkCloseHandle));

        const LONG result = transaction.Commit() ? NO_ERROR : ERROR_INVALID_FUNCTION;

        hidHooks = result == NO_ERROR;
        if (hidHooks)
            LOG_INFO("Win32 HID hooks installed");
        else
            LOG_ERROR("Win32 HID hook installation failed result:{}", result);
    }

    if (!rawHooks)
    {
        HookLifetime::Transaction<DetourBackend> transaction;

        transaction.Attach(reinterpret_cast<void**>(&o_GetRawInputData), reinterpret_cast<void*>(hkGetRawInputData));
        transaction.Attach(reinterpret_cast<void**>(&o_GetRawInputBuffer), reinterpret_cast<void*>(hkGetRawInputBuffer));
        transaction.Attach(reinterpret_cast<void**>(&o_RegisterRawInputDevices), reinterpret_cast<void*>(hkRegisterRawInputDevices));

        const LONG result = transaction.Commit() ? NO_ERROR : ERROR_INVALID_FUNCTION;

        rawHooks = result == NO_ERROR;
        if (rawHooks)
            LOG_INFO("Win32 raw input hooks installed");
        else
            LOG_ERROR("Win32 raw input installation failed result:{}", result);
    }

    if (!windowsHooks)
    {
        HookLifetime::Transaction<DetourBackend> transaction;

        transaction.Attach(reinterpret_cast<void**>(&o_SetWindowsHookExA), reinterpret_cast<void*>(hkSetWindowsHookExA));
        transaction.Attach(reinterpret_cast<void**>(&o_SetWindowsHookExW), reinterpret_cast<void*>(hkSetWindowsHookExW));
        transaction.Attach(reinterpret_cast<void**>(&o_UnhookWindowsHookEx), reinterpret_cast<void*>(hkUnhookWindowsHookEx));

        const LONG result = transaction.Commit() ? NO_ERROR : ERROR_INVALID_FUNCTION;

        windowsHooks = result == NO_ERROR;
        if (windowsHooks)
            LOG_INFO("Win32 windows input hooks installed");
        else
            LOG_ERROR("Win32 windows input installation failed result:{}", result);
    }

    if (!positionHooks)
    {
        HookLifetime::Transaction<DetourBackend> transaction;

        transaction.Attach(reinterpret_cast<void**>(&o_GetCursorPos), reinterpret_cast<void*>(hkGetCursorPos));
        transaction.Attach(reinterpret_cast<void**>(&o_SetCursorPos), reinterpret_cast<void*>(hkSetCursorPos));

        if (o_GetPhysicalCursorPos != nullptr)
            transaction.Attach(reinterpret_cast<void**>(&o_GetPhysicalCursorPos), reinterpret_cast<void*>(hkGetPhysicalCursorPos));

        if (o_SetPhysicalCursorPos != nullptr)
            transaction.Attach(reinterpret_cast<void**>(&o_SetPhysicalCursorPos), reinterpret_cast<void*>(hkSetPhysicalCursorPos));

        const LONG result = transaction.Commit() ? NO_ERROR : ERROR_INVALID_FUNCTION;

        positionHooks = result == NO_ERROR;
        if (positionHooks)
            LOG_INFO("Win32 position input hooks installed");
        else
            LOG_ERROR("Win32 position input installation failed result:{}", result);
    }

    // If CursorPos hooks are not installed, try to install
    // IAT (Import Address Table) hooks for CursorPos
    if (!positionHooks && !positionIATHooks)
        positionIATHooks = InstallCursorIatHooks();

    const bool hidReady = State::Instance().isRunningOnLinux || hidHooks;
    _state.HooksInstalled = messageHooks && keyStateHooks && getPosHooks && clipCursorHooks && message2Hooks &&
                            hidReady && rawHooks && windowsHooks && (positionHooks || positionIATHooks);

    return _state.HooksInstalled;
}

bool RemoveHooks()
{
    const bool hasDetours = messageHooks || keyStateHooks || getPosHooks || clipCursorHooks || message2Hooks || hidHooks || rawHooks || windowsHooks || positionHooks;
    if (hasDetours)
    {
        HookLifetime::Transaction<DetourBackend> transaction;
        if (messageHooks)
        {
            transaction.Detach(reinterpret_cast<void**>(&o_PeekMessageA), reinterpret_cast<void*>(hkPeekMessageA));
            transaction.Detach(reinterpret_cast<void**>(&o_PeekMessageW), reinterpret_cast<void*>(hkPeekMessageW));
            transaction.Detach(reinterpret_cast<void**>(&o_GetMessageA), reinterpret_cast<void*>(hkGetMessageA));
            transaction.Detach(reinterpret_cast<void**>(&o_GetMessageW), reinterpret_cast<void*>(hkGetMessageW));
        }
        if (keyStateHooks)
        {
            transaction.Detach(reinterpret_cast<void**>(&o_GetAsyncKeyState), reinterpret_cast<void*>(hkGetAsyncKeyState));
            transaction.Detach(reinterpret_cast<void**>(&o_GetKeyState), reinterpret_cast<void*>(hkGetKeyState));
            transaction.Detach(reinterpret_cast<void**>(&o_GetKeyboardState), reinterpret_cast<void*>(hkGetKeyboardState));
        }
        if (getPosHooks)
        {
            transaction.Detach(reinterpret_cast<void**>(&o_GetMessagePos), reinterpret_cast<void*>(hkGetMessagePos));
            transaction.Detach(reinterpret_cast<void**>(&o_GetMouseMovePointsEx), reinterpret_cast<void*>(hkGetMouseMovePointsEx));
        }
        if (clipCursorHooks)
        {
            transaction.Detach(reinterpret_cast<void**>(&o_ClipCursor), reinterpret_cast<void*>(hkClipCursor));
            transaction.Detach(reinterpret_cast<void**>(&o_GetClipCursor), reinterpret_cast<void*>(hkGetClipCursor));
        }
        if (message2Hooks)
        {
            transaction.Detach(reinterpret_cast<void**>(&o_SendInput), reinterpret_cast<void*>(hkSendInput));
            transaction.Detach(reinterpret_cast<void**>(&o_mouse_event), reinterpret_cast<void*>(hkmouse_event));
            transaction.Detach(reinterpret_cast<void**>(&o_PostMessageA), reinterpret_cast<void*>(hkPostMessageA));
            transaction.Detach(reinterpret_cast<void**>(&o_PostMessageW), reinterpret_cast<void*>(hkPostMessageW));
            transaction.Detach(reinterpret_cast<void**>(&o_SendMessageA), reinterpret_cast<void*>(hkSendMessageA));
            transaction.Detach(reinterpret_cast<void**>(&o_SendMessageW), reinterpret_cast<void*>(hkSendMessageW));
        }
        if (hidHooks)
        {
            transaction.Detach(reinterpret_cast<void**>(&o_CreateFileA), reinterpret_cast<void*>(hkCreateFileA));
            transaction.Detach(reinterpret_cast<void**>(&o_CreateFileW), reinterpret_cast<void*>(hkCreateFileW));
            transaction.Detach(reinterpret_cast<void**>(&o_ReadFile), reinterpret_cast<void*>(hkReadFile));
            transaction.Detach(reinterpret_cast<void**>(&o_DeviceIoControl), reinterpret_cast<void*>(hkDeviceIoControl));
            transaction.Detach(reinterpret_cast<void**>(&o_CloseHandle), reinterpret_cast<void*>(hkCloseHandle));
        }
        if (rawHooks)
        {
            transaction.Detach(reinterpret_cast<void**>(&o_GetRawInputData), reinterpret_cast<void*>(hkGetRawInputData));
            transaction.Detach(reinterpret_cast<void**>(&o_GetRawInputBuffer), reinterpret_cast<void*>(hkGetRawInputBuffer));
            transaction.Detach(reinterpret_cast<void**>(&o_RegisterRawInputDevices), reinterpret_cast<void*>(hkRegisterRawInputDevices));
        }
        if (windowsHooks)
        {
            transaction.Detach(reinterpret_cast<void**>(&o_SetWindowsHookExA), reinterpret_cast<void*>(hkSetWindowsHookExA));
            transaction.Detach(reinterpret_cast<void**>(&o_SetWindowsHookExW), reinterpret_cast<void*>(hkSetWindowsHookExW));
            transaction.Detach(reinterpret_cast<void**>(&o_UnhookWindowsHookEx), reinterpret_cast<void*>(hkUnhookWindowsHookEx));
        }
        if (positionHooks)
        {
            transaction.Detach(reinterpret_cast<void**>(&o_GetCursorPos), reinterpret_cast<void*>(hkGetCursorPos));
            transaction.Detach(reinterpret_cast<void**>(&o_SetCursorPos), reinterpret_cast<void*>(hkSetCursorPos));
            if (o_GetPhysicalCursorPos != nullptr)
                transaction.Detach(reinterpret_cast<void**>(&o_GetPhysicalCursorPos), reinterpret_cast<void*>(hkGetPhysicalCursorPos));
            if (o_SetPhysicalCursorPos != nullptr)
                transaction.Detach(reinterpret_cast<void**>(&o_SetPhysicalCursorPos), reinterpret_cast<void*>(hkSetPhysicalCursorPos));
        }
        if (!transaction.Commit())
        {
            LOG_WARN("Win32 input detach failed; retaining installed flags and trampolines");
            return false;
        }
        messageHooks = false;
        keyStateHooks = false;
        getPosHooks = false;
        clipCursorHooks = false;
        message2Hooks = false;
        hidHooks = false;
        rawHooks = false;
        windowsHooks = false;
        positionHooks = false;
    }
    if (positionIATHooks)
        positionIATHooks = !RemoveCursorIatHooks();
    if (positionIATHooks)
        return false;
    _state.HooksInstalled = false;
    return true;
}

} // namespace OptiInput
