#include "pch.h"
#include "input_system_internal.h"

#include <detours/detours.h>

namespace OptiInput
{
namespace
{
constexpr wchar_t GameInputModuleName[] = L"GameInput.dll";
constexpr wchar_t WindowsGamingInputModuleName[] = L"Windows.Gaming.Input.dll";
constexpr char GameInputCreateExportName[] = "GameInputCreate";

HMODULE GetAlreadyLoadedModule(const wchar_t* moduleName) { return GetModuleHandleW(moduleName); }

void RefreshGameInputModuleStateLocked()
{
    _state.GameInputModule = GetAlreadyLoadedModule(GameInputModuleName);
    _state.WindowsGamingInputModule = GetAlreadyLoadedModule(WindowsGamingInputModuleName);

    _state.GameInputModuleLoaded = _state.GameInputModule != nullptr;
    _state.WindowsGamingInputModuleLoaded = _state.WindowsGamingInputModule != nullptr;

    if (!_state.GameInputModuleLoaded)
    {
        _state.GameInputCreateExportFound = false;
        return;
    }

    _state.GameInputCreateExportFound = GetProcAddress(_state.GameInputModule, GameInputCreateExportName) != nullptr;
}

bool InstallGameInputCreateHookLocked()
{
    if (_state.GameInputCreateHookInstalled)
        return true;

    if (_state.GameInputCreateHookAttempted)
        return false;

    if (_state.GameInputModule == nullptr)
        return false;

    FARPROC proc = GetProcAddress(_state.GameInputModule, GameInputCreateExportName);

    if (proc == nullptr)
    {
        _state.GameInputCreateExportFound = false;
        return false;
    }
    if (!RetainInputCallbackModule(reinterpret_cast<const void*>(proc)) ||
        !RetainInputCallbackModule(reinterpret_cast<const void*>(&hkGameInputCreate)))
        return false;

    _state.GameInputCreateExportFound = true;
    _state.GameInputCreateHookAttempted = true;
    o_GameInputCreate = reinterpret_cast<GameInputCreate_t>(proc);

    HookLifetime::Transaction<DetourBackend> transaction;
    transaction.Attach(reinterpret_cast<void**>(&o_GameInputCreate), reinterpret_cast<void*>(hkGameInputCreate));

    const LONG result = transaction.Commit() ? NO_ERROR : ERROR_INVALID_FUNCTION;

    _state.GameInputCreateHookInstalled = result == NO_ERROR;

    if (!_state.GameInputCreateHookInstalled)
    {
        LOG_WARN("GameInputCreate hook installation failed result:{}", result);
        o_GameInputCreate = nullptr;
    }

    return _state.GameInputCreateHookInstalled;
}
} // namespace

void UpdateGameInputIntegrationLocked()
{
    RefreshGameInputModuleStateLocked();

    if (_state.GameInputModuleLoaded && _state.GameInputCreateExportFound)
        InstallGameInputCreateHookLocked();
}

bool RemoveGameInputHooksLocked()
{
    if (!_state.GameInputCreateHookInstalled)
        return true;
    if (o_GameInputCreate == nullptr)
        return false;
    HookLifetime::Transaction<DetourBackend> transaction;
    transaction.Detach(reinterpret_cast<void**>(&o_GameInputCreate), reinterpret_cast<void*>(hkGameInputCreate));
    if (!transaction.Commit())
    {
        LOG_WARN("GameInputCreate detach failed; retaining live trampoline");
        return false;
    }
    _state.GameInputCreateHookInstalled = false;
    return true;
}

HRESULT WINAPI hkGameInputCreate(void** gameInput)
{
    HookLifetime::CallbackLease callbackLease(NonDirectInputCallbackGate);
    {
        std::unique_lock lock(_state.Mutex);
        _state.GameInputCreateCallCount++;
    }

    HRESULT result = E_NOTIMPL;

    if (o_GameInputCreate != nullptr)
    {
        ScopedHookBypass bypass;
        result = o_GameInputCreate(gameInput);
    }

    {
        std::unique_lock lock(_state.Mutex);

        _state.GameInputLastCreateResult = result;

        if (SUCCEEDED(result))
        {
            _state.GameInputCreateSucceededCount++;

            if (gameInput != nullptr && *gameInput != nullptr)
                _state.GameInputInterfaceSeen = true;
        }
        else
        {
            _state.GameInputCreateFailedCount++;
        }
    }

    return result;
}

} // namespace OptiInput
