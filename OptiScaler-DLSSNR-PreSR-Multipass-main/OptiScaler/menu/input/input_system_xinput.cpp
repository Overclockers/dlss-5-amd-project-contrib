#include "pch.h"
#include "input_system_internal.h"

#include <detours/detours.h>

namespace OptiInput
{
namespace
{
constexpr const wchar_t* XInputModuleNames[] = {
    L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll", L"xinput1_2.dll", L"xinput1_1.dll",
};

constexpr char XInputGetStateExportName[] = "XInputGetState";
constexpr char XInputGetKeystrokeExportName[] = "XInputGetKeystroke";
constexpr char XInputSetStateExportName[] = "XInputSetState";
std::array<bool, XUSER_MAX_COUNT> keystrokeDrainPending {};

HMODULE FindLoadedXInputModule()
{
    for (const wchar_t* moduleName : XInputModuleNames)
    {
        HMODULE module = GetModuleHandleW(moduleName);

        if (module != nullptr)
            return module;
    }

    return nullptr;
}

bool ShouldBlockXInputLocked()
{
    return _state.Initialized && _state.Focused && ShouldApplyBlockingPolicyLocked() &&
           (_state.BlockKeyboard || _state.BlockMouse);
}

void FillNeutralXInputState(XINPUT_STATE* state)
{
    if (state == nullptr)
        return;

    const DWORD packetNumber = state->dwPacketNumber;
    *state = {};
    state->dwPacketNumber = packetNumber;
}

void ClearXInputHookPointersLocked()
{
    o_XInputGetState = nullptr;
    o_XInputGetStateEx = nullptr;
    o_XInputGetKeystroke = nullptr;
    o_XInputSetState = nullptr;

    _state.XInputGetStateHookInstalled = false;
    _state.XInputGetStateExHookInstalled = false;
    _state.XInputGetKeystrokeHookInstalled = false;
    _state.XInputSetStateHookInstalled = false;
}

bool ResolveXInputExportsLocked(HMODULE module)
{
    if (module == nullptr)
        return false;

    if (o_XInputGetState == nullptr)
        o_XInputGetState = reinterpret_cast<XInputGetState_t>(GetProcAddress(module, XInputGetStateExportName));

    if (o_XInputGetStateEx == nullptr)
        o_XInputGetStateEx = reinterpret_cast<XInputGetState_t>(GetProcAddress(module, MAKEINTRESOURCEA(100)));

    if (o_XInputGetKeystroke == nullptr)
        o_XInputGetKeystroke =
            reinterpret_cast<XInputGetKeystroke_t>(GetProcAddress(module, XInputGetKeystrokeExportName));

    if (o_XInputSetState == nullptr)
        o_XInputSetState = reinterpret_cast<XInputSetState_t>(GetProcAddress(module, XInputSetStateExportName));

    return o_XInputGetState != nullptr || o_XInputGetStateEx != nullptr || o_XInputGetKeystroke != nullptr ||
           o_XInputSetState != nullptr;
}
} // namespace

void MarkXInputKeystrokesForDrainLocked()
{
    if (_state.XInputGetKeystrokeHookInstalled)
        keystrokeDrainPending.fill(true);
}

void UpdateXInputIntegrationLocked()
{
    if (_state.XInputGetStateHookInstalled || _state.XInputGetStateExHookInstalled ||
        _state.XInputGetKeystrokeHookInstalled || _state.XInputSetStateHookInstalled)
    {
        return;
    }

    HMODULE module = FindLoadedXInputModule();
    _state.XInputModule = module;
    _state.XInputModuleLoaded = module != nullptr;

    if (module == nullptr)
        return;
    if (!RetainInputCallbackModule(reinterpret_cast<const void*>(module)) ||
        !RetainInputCallbackModule(reinterpret_cast<const void*>(&hkXInputGetState)))
        return;

    if (!ResolveXInputExportsLocked(module))
    {
        LOG_WARN("XInput module loaded but no supported exports found module:{}", static_cast<void*>(module));
        return;
    }

    HookLifetime::Transaction<DetourBackend> transaction;

    if (o_XInputGetState != nullptr)
        transaction.Attach(reinterpret_cast<void**>(&o_XInputGetState), reinterpret_cast<void*>(hkXInputGetState));

    if (o_XInputGetStateEx != nullptr)
        transaction.Attach(reinterpret_cast<void**>(&o_XInputGetStateEx), reinterpret_cast<void*>(hkXInputGetStateEx));

    if (o_XInputGetKeystroke != nullptr)
        transaction.Attach(reinterpret_cast<void**>(&o_XInputGetKeystroke), reinterpret_cast<void*>(hkXInputGetKeystroke));

    if (o_XInputSetState != nullptr)
        transaction.Attach(reinterpret_cast<void**>(&o_XInputSetState), reinterpret_cast<void*>(hkXInputSetState));

    const LONG result = transaction.Commit() ? NO_ERROR : ERROR_INVALID_FUNCTION;

    if (result != NO_ERROR)
    {
        LOG_ERROR("XInput hook installation failed result:{}", result);
        ClearXInputHookPointersLocked();
        return;
    }

    _state.XInputGetStateHookInstalled = o_XInputGetState != nullptr;
    _state.XInputGetStateExHookInstalled = o_XInputGetStateEx != nullptr;
    _state.XInputGetKeystrokeHookInstalled = o_XInputGetKeystroke != nullptr;
    _state.XInputSetStateHookInstalled = o_XInputSetState != nullptr;

    LOG_INFO("XInput hooks installed module:{} getState:{} getStateEx:{} getKeystroke:{} setState:{}",
             static_cast<void*>(module), _state.XInputGetStateHookInstalled ? 1 : 0,
             _state.XInputGetStateExHookInstalled ? 1 : 0, _state.XInputGetKeystrokeHookInstalled ? 1 : 0,
             _state.XInputSetStateHookInstalled ? 1 : 0);
}

bool RemoveXInputHooksLocked()
{
    if (!_state.XInputGetStateHookInstalled && !_state.XInputGetStateExHookInstalled &&
        !_state.XInputGetKeystrokeHookInstalled && !_state.XInputSetStateHookInstalled)
        return true;

    HookLifetime::Transaction<DetourBackend> transaction;
    if (_state.XInputGetStateHookInstalled)
        transaction.Detach(reinterpret_cast<void**>(&o_XInputGetState), reinterpret_cast<void*>(hkXInputGetState));
    if (_state.XInputGetStateExHookInstalled)
        transaction.Detach(reinterpret_cast<void**>(&o_XInputGetStateEx), reinterpret_cast<void*>(hkXInputGetStateEx));
    if (_state.XInputGetKeystrokeHookInstalled)
        transaction.Detach(reinterpret_cast<void**>(&o_XInputGetKeystroke), reinterpret_cast<void*>(hkXInputGetKeystroke));
    if (_state.XInputSetStateHookInstalled)
        transaction.Detach(reinterpret_cast<void**>(&o_XInputSetState), reinterpret_cast<void*>(hkXInputSetState));
    if (!transaction.Commit())
    {
        LOG_WARN("XInput detach failed; retaining installed flags and trampolines");
        return false;
    }
    // Restored API pointers remain callable by a callback dispatched before detach.
    _state.XInputGetStateHookInstalled = false;
    _state.XInputGetStateExHookInstalled = false;
    _state.XInputGetKeystrokeHookInstalled = false;
    _state.XInputSetStateHookInstalled = false;
    return true;
}

DWORD WINAPI hkXInputGetState(DWORD userIndex, XINPUT_STATE* state)
{
    HookLifetime::CallbackLease callbackLease(NonDirectInputCallbackGate);
    if (state == nullptr)
    {
        if (o_XInputGetState != nullptr)
        {
            ScopedHookBypass bypass;
            return o_XInputGetState(userIndex, state);
        }

        return ERROR_BAD_ARGUMENTS;
    }

    bool shouldBlock = false;

    {
        std::unique_lock lock(_state.Mutex);
        _state.XInputGetStateCallCount++;
        shouldBlock = ShouldBlockXInputLocked();

        if (!shouldBlock)
            _state.XInputGetStatePassedCount++;
    }

    if (o_XInputGetState == nullptr)
        return ERROR_DEVICE_NOT_CONNECTED;

    if (!shouldBlock)
    {
        ScopedHookBypass bypass;
        return o_XInputGetState(userIndex, state);
    }

    XINPUT_STATE realState {};
    DWORD result = ERROR_DEVICE_NOT_CONNECTED;

    {
        ScopedHookBypass bypass;
        result = o_XInputGetState(userIndex, &realState);
    }

    {
        std::unique_lock lock(_state.Mutex);
        _state.XInputGetStateBlockedCount++;
    }

    if (result != ERROR_SUCCESS)
    {
        *state = {};
        return result;
    }

    *state = realState;
    state->Gamepad = {}; // neutral connected controller only if it really exists

    return ERROR_SUCCESS;
}

DWORD WINAPI hkXInputGetStateEx(DWORD userIndex, XINPUT_STATE* state)
{
    HookLifetime::CallbackLease callbackLease(NonDirectInputCallbackGate);
    if (state == nullptr)
    {
        if (o_XInputGetStateEx != nullptr)
        {
            ScopedHookBypass bypass;
            return o_XInputGetStateEx(userIndex, state);
        }

        return ERROR_BAD_ARGUMENTS;
    }

    bool shouldBlock = false;

    {
        std::unique_lock lock(_state.Mutex);
        _state.XInputGetStateCallCount++;
        shouldBlock = ShouldBlockXInputLocked();

        if (!shouldBlock)
            _state.XInputGetStatePassedCount++;
    }

    if (o_XInputGetStateEx == nullptr)
        return ERROR_DEVICE_NOT_CONNECTED;

    if (!shouldBlock)
    {
        ScopedHookBypass bypass;
        return o_XInputGetStateEx(userIndex, state);
    }

    XINPUT_STATE realState {};
    DWORD result = ERROR_DEVICE_NOT_CONNECTED;

    {
        ScopedHookBypass bypass;
        result = o_XInputGetStateEx(userIndex, &realState);
    }

    {
        std::unique_lock lock(_state.Mutex);
        _state.XInputGetStateBlockedCount++;
    }

    if (result != ERROR_SUCCESS)
    {
        *state = {};
        return result;
    }

    *state = realState;
    state->Gamepad = {}; // neutral connected controller only if it really exists

    return ERROR_SUCCESS;
}

DWORD WINAPI hkXInputGetKeystroke(DWORD userIndex, DWORD reserved, PXINPUT_KEYSTROKE keystroke)
{
    HookLifetime::CallbackLease callbackLease(NonDirectInputCallbackGate);
    bool blocked = false;
    bool drain = false;
    {
        std::unique_lock lock(_state.Mutex);
        _state.XInputGetKeystrokeCallCount++;
        blocked = ShouldBlockXInputLocked();
        if (userIndex < XUSER_MAX_COUNT)
        {
            drain = keystrokeDrainPending[userIndex];
            keystrokeDrainPending[userIndex] = false;
        }
        else if (userIndex == XUSER_INDEX_ANY)
        {
            drain = std::any_of(keystrokeDrainPending.begin(), keystrokeDrainPending.end(), [](bool value) { return value; });
            keystrokeDrainPending.fill(false);
        }
        if (blocked)
            _state.XInputGetKeystrokeBlockedCount++;
        else
            _state.XInputGetKeystrokePassedCount++;
    }
    if (o_XInputGetKeystroke == nullptr)
        return ERROR_EMPTY;
    ScopedHookBypass bypass;
    if (blocked || drain)
    {
        XINPUT_KEYSTROKE discarded {};
        // Never call foreign input code with the shared state mutex held.
        unsigned consumed = 0;
        while (consumed < 1024 && o_XInputGetKeystroke(userIndex, reserved, &discarded) == ERROR_SUCCESS)
            ++consumed;
        if (consumed == 1024)
        {
            std::unique_lock lock(_state.Mutex);
            if (userIndex < XUSER_MAX_COUNT)
                keystrokeDrainPending[userIndex] = true;
            else if (userIndex == XUSER_INDEX_ANY)
                keystrokeDrainPending.fill(true);
            if (keystroke != nullptr)
                *keystroke = {};
            return ERROR_EMPTY; // Continue draining next call; do not leak stale input.
        }
    }
    if (blocked)
    {
        if (keystroke != nullptr)
            *keystroke = {};
        return ERROR_EMPTY;
    }
    return o_XInputGetKeystroke(userIndex, reserved, keystroke);
}

DWORD WINAPI hkXInputSetState(DWORD userIndex, XINPUT_VIBRATION* vibration)
{
    HookLifetime::CallbackLease callbackLease(NonDirectInputCallbackGate);
    bool shouldBlock = false;

    {
        std::unique_lock lock(_state.Mutex);
        _state.XInputSetStateCallCount++;
        shouldBlock = ShouldBlockXInputLocked();

        if (!shouldBlock)
            _state.XInputSetStatePassedCount++;
    }

    if (o_XInputSetState == nullptr)
        return ERROR_DEVICE_NOT_CONNECTED;

    if (!shouldBlock)
    {
        ScopedHookBypass bypass;
        return o_XInputSetState(userIndex, vibration);
    }

    XINPUT_VIBRATION zeroVibration {};
    DWORD result = ERROR_DEVICE_NOT_CONNECTED;

    {
        ScopedHookBypass bypass;
        result = o_XInputSetState(userIndex, &zeroVibration);
    }

    {
        std::unique_lock lock(_state.Mutex);
        _state.XInputSetStateBlockedCount++;
    }

    return result;
}

} // namespace OptiInput
